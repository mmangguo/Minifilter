/*++

Module Name:

    Encrypt.c

Abstract:

    This is the main module of the Encrypt miniFilter driver.

Environment:

    Kernel mode

--*/

#include "global.h"
#include "filefunc.h"
#include "context.h"
#include "swapbuffers.h"
#include "commport.h"
#include "cryptography.h"
#include "processverify.h"
#include "linkedList.h"
#include "privilegeendecrypt.h"
#include "commport.h"

#pragma prefast(disable:__WARNING_ENCODE_MEMBER_FUNCTION_POINTER, "Not valid for kernel mode drivers")

#pragma warning(disable:4996)


PFLT_FILTER gFilterHandle;
ULONG_PTR OperationStatusCtx = 1;
EptQueryInformationProcess pEptQueryInformationProcess = NULL;

#define PTDBG_TRACE_ROUTINES            0x00000001
#define PTDBG_TRACE_OPERATION_STATUS    0x00000002

ULONG gTraceFlags = 0;


#define PT_DBG_PRINT( _dbgLevel, _string )          \
    (FlagOn(gTraceFlags,(_dbgLevel)) ?              \
        DbgPrint _string :                          \
        ((int)0))


//
//  Assign text sections for each routine.
//

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, EncryptUnload)
#pragma alloc_text(PAGE, EncryptInstanceQueryTeardown)
#pragma alloc_text(PAGE, EncryptInstanceSetup)
#pragma alloc_text(PAGE, EncryptInstanceTeardownStart)
#pragma alloc_text(PAGE, EncryptInstanceTeardownComplete)
#pragma alloc_text(PAGE, EptContextCleanUp)
#endif

//
//  operation registration
//

CONST FLT_CONTEXT_REGISTRATION Context[] = {

    { FLT_VOLUME_CONTEXT,
      0,
      EptContextCleanUp,
      sizeof(VOLUME_CONTEXT),
      VOLUME_CONTEXT_TAG 
    },

    { FLT_STREAM_CONTEXT,
      0,
      EptContextCleanUp,
      sizeof(EPT_STREAM_CONTEXT),
      STREAM_CONTEXT_TAG 
    },

    { FLT_CONTEXT_END }

};

CONST FLT_OPERATION_REGISTRATION Callbacks[] = {

    { IRP_MJ_CREATE,
      0,
      EncryptPreCreate,
      EncryptPostCreate 
    },

    { IRP_MJ_READ,
      0,
      EncryptPreRead,
      EncryptPostRead
    },

    { IRP_MJ_WRITE,
      0,
      EncryptPreWrite,
      EncryptPostWrite 
    },

    { IRP_MJ_QUERY_INFORMATION,
      0,
      EncryptPreQueryInformation,
      EncryptPostQueryInformation
    },

    { IRP_MJ_SET_INFORMATION,
      0,
      EncryptPreSetInformation,
      EncryptPostSetInformation
    },
        
    { IRP_MJ_CLEANUP,
      0,
      EncryptPreCleanUp,
      EncryptPostCleanUp
    },

    { IRP_MJ_CLOSE,
      0,
      EncryptPreClose,
      EncryptPostClose
    },

    { IRP_MJ_OPERATION_END }
};

//
//  This defines what we want to filter with FltMgr
//

CONST FLT_REGISTRATION FilterRegistration = {

    sizeof( FLT_REGISTRATION ),         //  Size
    FLT_REGISTRATION_VERSION,           //  Version
    0,                                  //  Flags

    Context,                               //  Context
    Callbacks,                          //  Operation callbacks

    EncryptUnload,                           //  MiniFilterUnload

    EncryptInstanceSetup,                    //  InstanceSetup
    EncryptInstanceQueryTeardown,            //  InstanceQueryTeardown
    EncryptInstanceTeardownStart,            //  InstanceTeardownStart
    EncryptInstanceTeardownComplete,         //  InstanceTeardownComplete

    NULL,                               //  GenerateFileName
    NULL,                               //  GenerateDestinationFileName
    NULL                                //  NormalizeNameComponent

};



NTSTATUS
EncryptInstanceSetup (
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_SETUP_FLAGS Flags,
    _In_ DEVICE_TYPE VolumeDeviceType,
    _In_ FLT_FILESYSTEM_TYPE VolumeFilesystemType
    )
{
    UNREFERENCED_PARAMETER( Flags );
    UNREFERENCED_PARAMETER( VolumeDeviceType );
    UNREFERENCED_PARAMETER( VolumeFilesystemType );


    NTSTATUS Status;
    PVOLUME_CONTEXT VolumeContext = NULL;
    ULONG LengthReturned;
    UCHAR VolPropBuffer[sizeof(FLT_VOLUME_PROPERTIES) + 512];
    PFLT_VOLUME_PROPERTIES VolProp = (PFLT_VOLUME_PROPERTIES)VolPropBuffer;

    PAGED_CODE();

    try {

        Status = FltAllocateContext(gFilterHandle, FLT_VOLUME_CONTEXT, sizeof(VOLUME_CONTEXT), NonPagedPool, &VolumeContext);

        if (!NT_SUCCESS(Status)) {

            DbgPrint("pVolumeContext FltAllocateContext failed!.\n");
            leave;
        }

        RtlZeroMemory(VolumeContext, sizeof(VOLUME_CONTEXT));


        Status = FltGetVolumeProperties(FltObjects->Volume, VolProp, sizeof(VolPropBuffer), &LengthReturned);

        if (!NT_SUCCESS(Status)) {

            DbgPrint("VolProp FltGetVolumeProperties failed!.\n");
            leave;
        }

        //  we will pick a minimum sector size if a sector size is not
        //  specified. MIN_SECTOR_SIZE = 0x200.
        VolumeContext->SectorSize = max(VolProp->SectorSize, 0x200);

        FltSetVolumeContext(FltObjects->Volume, FLT_SET_CONTEXT_KEEP_IF_EXISTS, VolumeContext, NULL);

        Status = STATUS_SUCCESS;

    }
    finally {

        if (VolumeContext) {

            FltReleaseContext(VolumeContext);
        }

    }


    return Status;
}


NTSTATUS
EncryptInstanceQueryTeardown (
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags
    )
/*++

Routine Description:

    This is called when an instance is being manually deleted by a
    call to FltDetachVolume or FilterDetach thereby giving us a
    chance to fail that detach request.

    If this routine is not defined in the registration structure, explicit
    detach requests via FltDetachVolume or FilterDetach will always be
    failed.

Arguments:

    FltObjects - Pointer to the FLT_RELATED_OBJECTS data structure containing
        opaque handles to this filter, instance and its associated volume.

    Flags - Indicating where this detach request came from.

Return Value:

    Returns the status of this operation.

--*/
{
    UNREFERENCED_PARAMETER( FltObjects );
    UNREFERENCED_PARAMETER( Flags );

    PAGED_CODE();

    PT_DBG_PRINT( PTDBG_TRACE_ROUTINES,
                  ("Encrypt!EncryptInstanceQueryTeardown: Entered\n") );

    return STATUS_SUCCESS;
}


VOID
EncryptInstanceTeardownStart (
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_TEARDOWN_FLAGS Flags
    )
/*++

Routine Description:

    This routine is called at the start of instance teardown.

Arguments:

    FltObjects - Pointer to the FLT_RELATED_OBJECTS data structure containing
        opaque handles to this filter, instance and its associated volume.

    Flags - Reason why this instance is being deleted.

Return Value:

    None.

--*/
{
    UNREFERENCED_PARAMETER( FltObjects );
    UNREFERENCED_PARAMETER( Flags );

    PAGED_CODE();

    PT_DBG_PRINT( PTDBG_TRACE_ROUTINES,
                  ("Encrypt!EncryptInstanceTeardownStart: Entered\n") );
}


VOID
EncryptInstanceTeardownComplete (
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_TEARDOWN_FLAGS Flags
    )
/*++

Routine Description:

    This routine is called at the end of instance teardown.

Arguments:

    FltObjects - Pointer to the FLT_RELATED_OBJECTS data structure containing
        opaque handles to this filter, instance and its associated volume.

    Flags - Reason why this instance is being deleted.

Return Value:

    None.

--*/
{
    UNREFERENCED_PARAMETER( FltObjects );
    UNREFERENCED_PARAMETER( Flags );

    PAGED_CODE();

    PT_DBG_PRINT( PTDBG_TRACE_ROUTINES,
                  ("Encrypt!EncryptInstanceTeardownComplete: Entered\n") );
}


VOID
EptContextCleanUp(
    _In_ PFLT_CONTEXT context,
    _In_ FLT_CONTEXT_TYPE ContextType
)
{
    PEPT_STREAM_CONTEXT StreamContext = NULL;

    switch (ContextType) {

    case FLT_STREAM_CONTEXT:
    {
        StreamContext = (PEPT_STREAM_CONTEXT)context;

        if (NULL != StreamContext->Resource) {

            ExDeleteResourceLite(StreamContext->Resource);
            ExFreePoolWithTag(StreamContext->Resource, STREAM_CONTEXT_TAG);

            StreamContext->Resource = NULL;
        }

        if (NULL != StreamContext->FileName.Buffer)
        {
            ExFreePoolWithTag(StreamContext->FileName.Buffer, STREAM_CONTEXT_TAG);
            StreamContext->FileName.Buffer = NULL;
        }

        break;
    }
    case FLT_STREAMHANDLE_CONTEXT:
    {
        break;
    }

    }
}

/*************************************************************************
    MiniFilter initialization and unload routines.
*************************************************************************/

NTSTATUS
DriverEntry (
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
/*++

Routine Description:

    This is the initialization routine for this miniFilter driver.  This
    registers with FltMgr and initializes all global data structures.

Arguments:

    DriverObject - Pointer to driver object created by the system to
        represent this driver.

    RegistryPath - Unicode string identifying where the parameters for this
        driver are located in the registry.

Return Value:

    Routine can return non success error codes.

--*/
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER( RegistryPath );


    //�õ�ZwQueryInformationProcess()����ָ��
    UNICODE_STRING FuncName;
    RtlInitUnicodeString(&FuncName, L"ZwQueryInformationProcess");
    pEptQueryInformationProcess = (EptQueryInformationProcess)(ULONG_PTR)MmGetSystemRoutineAddress(&FuncName);

    //���̼��ܲ��������һЩ��ʼ��
    InitializeListHead(&ProcessRulesListHead);
    KeInitializeSpinLock(&ProcessRulesListSpinLock);
    ExInitializeResourceLite(&ProcessRulesListResource);

    /*�����ȳ�ʼ��һ������*/
#ifdef DBG
   
    PEPT_PROCESS_RULES ProcessRules;
    ProcessRules = ExAllocatePoolWithTag(PagedPool, sizeof(EPT_PROCESS_RULES), PROCESS_RULES_BUFFER_TAG);
    if (!ProcessRules)
    {
        DbgPrint("[DriverEntry]->ExAllocatePoolWithTag ProcessRules failed.\n");
        return 0;
    }
    RtlZeroMemory(ProcessRules, sizeof(EPT_PROCESS_RULES));


    RtlMoveMemory(ProcessRules->TargetProcessName, "notepad.exe", sizeof("notepad.exe"));
    RtlMoveMemory(ProcessRules->TargetExtension, "txt,", sizeof("txt,"));
    ProcessRules->count = 1;
    ProcessRules->IsCheckHash = FALSE;
    ProcessRules->Access = EPT_PR_ACCESS_READ_WRITE;
    //ProcessRules->Access = EPT_PR_ACCESS_BACKUP_RESTORE;
    //ProcessRules->Access = EPT_PR_ACCESS_NO_ACCESS;

    ULONGLONG Hash[4];
    Hash[0] = 0xa28438e1388f272a;
    Hash[1] = 0x52559536d99d65ba;
    Hash[2] = 0x15b1a8288be1200e;
    Hash[3] = 0x249851fdf7ee6c7e;

    ULONGLONG TempHash;
    RtlZeroMemory(ProcessRules->Hash, sizeof(ProcessRules->Hash));

    for (ULONG i = 0; i < 4; i++)
    {
        TempHash = Hash[i];
        for (ULONG j = 0; j < 8; j++)
        {
            ProcessRules->Hash[8 * (i + 1) - 1 - j] = TempHash % 256;
            TempHash = TempHash / 256;
        }

    }


    ExInterlockedInsertTailList(&ProcessRulesListHead, &ProcessRules->ListEntry, &ProcessRulesListSpinLock);
#endif // DEBUG

    //�����ں˺�������������ں��߳�ͬ��
    KeInitializeEvent(&g_SynchronizationEvent, SynchronizationEvent, TRUE);

    //
    //  Register with FltMgr to tell it our callback routines
    //

    status = FltRegisterFilter( DriverObject,
                                &FilterRegistration,
                                &gFilterHandle );

    FLT_ASSERT( NT_SUCCESS( status ) );

    if (NT_SUCCESS( status )) {

        //
        //  Start filtering i/o
        //

        status = FltStartFiltering( gFilterHandle );

        if (!NT_SUCCESS( status )) {

            FltUnregisterFilter( gFilterHandle );
        }

        if (!EptInitCommPort())
        {
            FltUnregisterFilter(gFilterHandle);
        }

        AesInitVar.Flag = EptAesInithKey();

        if (!AesInitVar.Flag)
        {
            FltUnregisterFilter(gFilterHandle);
            EptCloseCommPort();
        }

    }

    return status;
}

NTSTATUS
EncryptUnload (
    _In_ FLT_FILTER_UNLOAD_FLAGS Flags
    )
/*++

Routine Description:

    This is the unload routine for this miniFilter driver. This is called
    when the minifilter is about to be unloaded. We can fail this unload
    request if this is not a mandatory unload indicated by the Flags
    parameter.

Arguments:

    Flags - Indicating if this is a mandatory unload.

Return Value:

    Returns STATUS_SUCCESS.

--*/
{
    UNREFERENCED_PARAMETER( Flags );

    PAGED_CODE();

    PT_DBG_PRINT( PTDBG_TRACE_ROUTINES,
                  ("Encrypt!EncryptUnload: Entered\n") );

    EptCloseCommPort();

    if (gFilterHandle)
    {
        FltUnregisterFilter(gFilterHandle);
    }

    EptAesCleanUp();

    EptListCleanUp();

    return STATUS_SUCCESS;
}


/*************************************************************************
    MiniFilter callback routines.
*************************************************************************/
FLT_PREOP_CALLBACK_STATUS
EncryptPreCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    ) 
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);


    PEPT_STREAM_CONTEXT StreamContext;
    NTSTATUS Status;

    if (FlagOn(Data->Iopb->Parameters.Create.Options, FILE_DIRECTORY_FILE)) 
    {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    //�ж��Ƿ�ΪĿ����չ������һ��ɸѡ�����ٺ�������
    if (EPT_STATUS_TARGET_MATCH != EptIsTargetExtension(Data))
    {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    //������Ȩ��
    Status = EptIsTargetProcess(Data);

    if (!FlagOn(Status, (EPT_PR_ACCESS_READ_WRITE)))
    {
        if (FlagOn(Status, (EPT_PR_ACCESS_NO_ACCESS)))
        {
            return FLT_PREOP_COMPLETE;
        }
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }


    DbgPrint("EncryptPreCreate->hit.\n");

    if (!EptCreateContext(&StreamContext, FLT_STREAM_CONTEXT)) {

        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }


    *CompletionContext = StreamContext;

    return FLT_PREOP_SUCCESS_WITH_CALLBACK;
}


FLT_POSTOP_CALLBACK_STATUS
EncryptPostCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
)
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);
    UNREFERENCED_PARAMETER(Flags);

    PAGED_CODE();

    PEPT_STREAM_CONTEXT StreamContext;
    NTSTATUS Status;

    StreamContext = CompletionContext;

    if (!EptGetOrSetContext(FltObjects->Instance, FltObjects->FileObject, &StreamContext, FLT_STREAM_CONTEXT))
    {
        if (NULL != StreamContext)
        {
            FltReleaseContext(StreamContext);
            StreamContext = NULL;
        }
        //DbgPrint("[EncryptPostCreate]->EptGetOrSetContext failed.\n");
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    /*Pointer to a file object for the file that the data is to be read from.
    This file object must be currently open.Calling FltReadFile when the file object is not yet open or is no longer open
    (for example, in a pre - create or post - cleanup callback routine) causes the system to ASSERT on a checked build.
    This parameter is requiredand cannot be NULL.*/

    //�ж��Ƿ�Ϊ�½����ļ�������д�����ݵ���������д����ܱ�ʶͷ
    //����Ȳ����½��м���ͷ���ֲ������м���ͷ��
    //˵����Ŀ����̴򿪵���ͨ�ļ��������д�����򣬷ŵ�PreClose��ȥ������Ӽ���ͷ

    if (EPT_DONT_HAVE_ENCRYPT_HEADER == EptIsTargetFile(FltObjects))
    {
        Status = EptWriteEncryptHeader(&Data, FltObjects);

        if (EPT_WRITE_ENCRYPT_HEADER != Status)
        {
            //������Ҫд�����ͷ���ļ����������¼����PreClose���޸�
            if (EPT_TO_APPEND_ENCRYPT_HEADER == Status)
            {
                //PFLT_FILE_NAME_INFORMATION FileNameInfo;

                //Status = FltGetFileNameInformation(Data, FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_ALWAYS_ALLOW_CACHE_LOOKUP, &FileNameInfo);

                //if (!NT_SUCCESS(Status)) {

                //    DbgPrint("EncryptPostCreate->FltGetFileNameInformation failed. Status = %x\n", Status);

                //    if (NULL != StreamContext)
                //    {
                //        FltReleaseContext(StreamContext);
                //        StreamContext = NULL;
                //    }
                //    return FLT_POSTOP_FINISHED_PROCESSING;
                //}

                //Status = FltParseFileNameInformation(FileNameInfo);

                //if (!NT_SUCCESS(Status)) {

                //    DbgPrint("EncryptPostCreate->FltParseFileNameInformation failed. Status = %x\n", Status);

                //    if (NULL != StreamContext)
                //    {
                //        FltReleaseContext(StreamContext);
                //        StreamContext = NULL;
                //    }

                //    if (NULL != FileNameInfo)
                //    {
                //        FltReleaseFileNameInformation(FileNameInfo);
                //        FileNameInfo = NULL;
                //    }

                //    return FLT_POSTOP_FINISHED_PROCESSING;
                //}
                //
                ////FileName = \Device\HarddiskVolume3\Desktop\cache.txt

                ////ȡд��
                //ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);

                //StreamContext->AppendHeader = EPT_TO_APPEND_ENCRYPT_HEADER;

                //RtlMoveMemory(StreamContext->FileName.Buffer, FileNameInfo->Name.Buffer, FileNameInfo->Name.Length);
                //StreamContext->FileName.Length = FileNameInfo->Name.Length;

                //StreamContext->FileSize = (LONGLONG)EptGetFileSize(FltObjects->Instance, FltObjects->FileObject);

                //DbgPrint("EncryptPostCreate append encrypt header OrigFileSize = %d\n", StreamContext->FileSize);

                //ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);

                //if (NULL != FileNameInfo)
                //{
                //    FltReleaseFileNameInformation(FileNameInfo);
                //    FileNameInfo = NULL;
                //}
            }
          
            if (NULL != StreamContext)
            {
                FltReleaseContext(StreamContext);
                StreamContext = NULL;
            }
            return FLT_POSTOP_FINISHED_PROCESSING;
            
        }
    }

    DbgPrint("EncryptPostCreate->hit.\n");

    //������˵���ļ��м��ܱ�ʶͷ������StreamContext��ʶλ
    ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);

    StreamContext->FlagExist = EPT_ENCRYPT_FLAG_EXIST;

    ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);

    DbgPrint("EncryptPostCreate->Set StreamContext->FlagExist\n");
    
    if (NULL != StreamContext)
    {
        FltReleaseContext(StreamContext);
        StreamContext = NULL;
    }
    
    DbgPrint("\n");

    return FLT_POSTOP_FINISHED_PROCESSING;
}


FLT_PREOP_CALLBACK_STATUS
EncryptPreRead(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
)
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);


    PEPT_STREAM_CONTEXT StreamContext;
    NTSTATUS Status;

    //������Ȩ��
    Status = EptIsTargetProcess(Data);

    
    if (!FlagOn(Status, (EPT_PR_ACCESS_READ_WRITE)))
    {
        if (FlagOn(Status, (EPT_PR_ACCESS_NO_ACCESS)))
        {
            return FLT_PREOP_COMPLETE;
        }
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (Data->Iopb->Parameters.Read.Length == 0)
    {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (FlagOn(Data->Flags, FLTFL_CALLBACK_DATA_FAST_IO_OPERATION))
    {
        return FLT_PREOP_DISALLOW_FASTIO;
    }

    if (!EptCreateContext(&StreamContext, FLT_STREAM_CONTEXT)) {

        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (!EptGetOrSetContext(FltObjects->Instance, FltObjects->FileObject, &StreamContext, FLT_STREAM_CONTEXT))
    {
        if (NULL != StreamContext)
        {
            FltReleaseContext(StreamContext);
            StreamContext = NULL;
        }
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (!StreamContext->FlagExist) 
    {
        if (NULL != StreamContext)
        {
            FltReleaseContext(StreamContext);
            StreamContext = NULL;
        }
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (NULL != StreamContext)
    {
        FltReleaseContext(StreamContext);
        StreamContext = NULL;
    }

    DbgPrint("EncryptPreRead->hit. Data->Iopb->IrpFlags = 0x%x ReadLength = 0x%x.\n", Data->Iopb->IrpFlags, Data->Iopb->Parameters.Read.Length);

    if (!FlagOn(Data->Iopb->IrpFlags, (IRP_PAGING_IO | IRP_SYNCHRONOUS_PAGING_IO | IRP_NOCACHE)))
    {
        //0x60900
        Data->Iopb->Parameters.Read.ByteOffset.QuadPart += FILE_FLAG_SIZE;
        FltSetCallbackDataDirty(Data);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    DbgPrint("EncryptPreRead->enter. Data->Iopb->IrpFlags = 0x%x ReadLength = %d.\n", Data->Iopb->IrpFlags, Data->Iopb->Parameters.Read.Length);

    PreReadSwapBuffers(&Data, FltObjects, CompletionContext);

    if (!FlagOn(Status, (EPT_PR_NOTEPAD_PLUS_PLUS)))
    {
        Data->Iopb->Parameters.Read.ByteOffset.QuadPart += FILE_FLAG_SIZE;
    }

    FltSetCallbackDataDirty(Data);

    return FLT_PREOP_SUCCESS_WITH_CALLBACK;
}


FLT_POSTOP_CALLBACK_STATUS
EncryptPostRead(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
)
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);
    UNREFERENCED_PARAMETER(Flags);

    PAGED_CODE();

    DbgPrint("[EncryptPostRead]->hit.\n");

    PostReadSwapBuffers(&Data, FltObjects, CompletionContext, Flags);

    DbgPrint("\n");

    return FLT_POSTOP_FINISHED_PROCESSING;
}


FLT_PREOP_CALLBACK_STATUS
EncryptPreWrite(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
)
{

    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);

    
    PEPT_STREAM_CONTEXT StreamContext;
    NTSTATUS Status;

    if (Data->Iopb->Parameters.Write.Length == 0)
    {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    //������Ȩ��
    Status = EptIsTargetProcess(Data);

    if (!FlagOn(Status, (EPT_PR_ACCESS_READ_WRITE)))
    {
        if (FlagOn(Status, (EPT_PR_ACCESS_NO_ACCESS)))
        {
            return FLT_PREOP_COMPLETE;
        }
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }


    if (FlagOn(Data->Flags, FLTFL_CALLBACK_DATA_FAST_IO_OPERATION))
    {
        return FLT_PREOP_DISALLOW_FASTIO;
    }

    if (!EptCreateContext(&StreamContext, FLT_STREAM_CONTEXT)) {
    
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (!EptGetOrSetContext(FltObjects->Instance, FltObjects->FileObject, &StreamContext, FLT_STREAM_CONTEXT))
    {
        if (NULL != StreamContext)
        {
            FltReleaseContext(StreamContext);
            StreamContext = NULL;
        }
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (!StreamContext->FlagExist) {

        if (NULL != StreamContext)
        {
            FltReleaseContext(StreamContext);
            StreamContext = NULL;
        }
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (NULL != StreamContext)
    {
        FltReleaseContext(StreamContext);
        StreamContext = NULL;
    }

    if (!FlagOn(Data->Iopb->IrpFlags, (IRP_PAGING_IO | IRP_SYNCHRONOUS_PAGING_IO | IRP_NOCACHE)))
    {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    DbgPrint("EncryptPreWrite->hit.\n");

    PreWriteSwapBuffers(&Data, FltObjects, CompletionContext);

    FltSetCallbackDataDirty(Data);

    return FLT_PREOP_SUCCESS_WITH_CALLBACK;
}


FLT_POSTOP_CALLBACK_STATUS
EncryptPostWrite (
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
    )
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);
    UNREFERENCED_PARAMETER(Flags);

    PAGED_CODE();

    DbgPrint("EncryptPostWrite->hit.\n");

    PSWAP_BUFFER_CONTEXT SwapWriteContext = CompletionContext;

    if (SwapWriteContext->NewBuffer != NULL)
        FltFreePoolAlignedWithTag(FltObjects->Instance, SwapWriteContext->NewBuffer, SWAP_WRITE_BUFFER_TAG);

    //if (SwapWriteContext->NewMdlAddress != NULL)
    //    IoFreeMdl(SwapWriteContext->NewMdlAddress);

    if (SwapWriteContext != NULL) {
        ExFreePool(SwapWriteContext);
    }


    DbgPrint("\n");
    
    return FLT_POSTOP_FINISHED_PROCESSING;
}


FLT_PREOP_CALLBACK_STATUS
EncryptPreQueryInformation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
)
{

    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);

    PEPT_STREAM_CONTEXT StreamContext;
    NTSTATUS Status;


    //������Ȩ��
    Status = EptIsTargetProcess(Data);

    if (!FlagOn(Status, (EPT_PR_ACCESS_READ_WRITE)))
    {
        if (FlagOn(Status, (EPT_PR_ACCESS_NO_ACCESS)))
        {
            return FLT_PREOP_COMPLETE;
        }
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (FlagOn(Status, (EPT_PR_NOTEPAD_PLUS_PLUS)))
    {
        //DbgPrint("EncryptPreQueryInformation->notepad++ hit.\n");
    }

    if (FlagOn(Data->Flags, FLTFL_CALLBACK_DATA_FAST_IO_OPERATION))
    {
        return FLT_PREOP_DISALLOW_FASTIO;
    }

    if (!EptCreateContext(&StreamContext, FLT_STREAM_CONTEXT)) 
    {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    *CompletionContext = StreamContext;

    return FLT_PREOP_SUCCESS_WITH_CALLBACK;

}


FLT_POSTOP_CALLBACK_STATUS
EncryptPostQueryInformation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
)
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);
    UNREFERENCED_PARAMETER(Flags);

    PAGED_CODE();

    PEPT_STREAM_CONTEXT StreamContext;
    LONGLONG FileOffset = 0;

    StreamContext = CompletionContext;

    if (!EptGetOrSetContext(FltObjects->Instance, FltObjects->FileObject, &StreamContext, FLT_STREAM_CONTEXT))
    {
        if (NULL != StreamContext)
        {
            FltReleaseContext(StreamContext);
            StreamContext = NULL;
        }
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    if (!StreamContext->FlagExist) 
    {
        if (NULL != StreamContext)
        {
            FltReleaseContext(StreamContext);
            StreamContext = NULL;
        }
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    DbgPrint("EncryptPostQueryInformation->FileInformationClass = %d.\n", Data->Iopb->Parameters.QueryFileInformation.FileInformationClass);

    PVOID InfoBuffer = Data->Iopb->Parameters.QueryFileInformation.InfoBuffer;

    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(StreamContext->Resource, TRUE);

    if (StreamContext->FileSize > 0 &&(StreamContext->FileSize % AES_BLOCK_SIZE != 0))
    {
        FileOffset = (StreamContext->FileSize / AES_BLOCK_SIZE + 1) * AES_BLOCK_SIZE - StreamContext->FileSize;
    }
    else if (StreamContext->FileSize > 0 && (StreamContext->FileSize % AES_BLOCK_SIZE == 0))
    {
        FileOffset = 0;
    }

    ExReleaseResourceLite(StreamContext->Resource);
    KeLeaveCriticalRegion();

    //DbgPrint("[EncryptPostQueryInformation]->FileOffset = %d.\n", FileOffset);

    switch (Data->Iopb->Parameters.QueryFileInformation.FileInformationClass) {

    case FileStandardInformation:
    {
        PFILE_STANDARD_INFORMATION Info = (PFILE_STANDARD_INFORMATION)InfoBuffer;
        DbgPrint("EncryptPostQueryInformation->origin AllocationSize = %d EndOfFile = %d.\n", Info->AllocationSize.QuadPart, Info->EndOfFile.QuadPart);
        Info->EndOfFile.QuadPart = Info->EndOfFile.QuadPart - FILE_FLAG_SIZE - FileOffset;
        Info->AllocationSize.QuadPart = Info->AllocationSize.QuadPart - FILE_FLAG_SIZE;
        DbgPrint("EncryptPostQueryInformation->AllocationSize = %d EndOfFile = %d.\n", Info->AllocationSize.QuadPart, Info->EndOfFile.QuadPart);
        break;
    }
    case FileAllInformation:
    {
        PFILE_ALL_INFORMATION Info = (PFILE_ALL_INFORMATION)InfoBuffer;
        if (Data->IoStatus.Information >=
            sizeof(FILE_BASIC_INFORMATION) +
            sizeof(FILE_STANDARD_INFORMATION))
        {
            if (Info->StandardInformation.AllocationSize.QuadPart > FILE_FLAG_SIZE)
            {
                Info->StandardInformation.AllocationSize.QuadPart -= FILE_FLAG_SIZE;
            }
            
            Info->StandardInformation.EndOfFile.QuadPart = Info->StandardInformation.EndOfFile.QuadPart - FILE_FLAG_SIZE - FileOffset;

        }
        break;
    }
    case FileAllocationInformation:
    {
        PFILE_ALLOCATION_INFORMATION Info = (PFILE_ALLOCATION_INFORMATION)InfoBuffer;
        Info->AllocationSize.QuadPart = Info->AllocationSize.QuadPart - FILE_FLAG_SIZE;
        DbgPrint("EncryptPostQueryInformation->FileAllocationInformation AllocationSize = %d.\n", Info->AllocationSize.QuadPart);
        break;
    }
    case FileEndOfFileInformation:
    {
        PFILE_END_OF_FILE_INFORMATION Info = (PFILE_END_OF_FILE_INFORMATION)InfoBuffer;
        Info->EndOfFile.QuadPart = Info->EndOfFile.QuadPart - FILE_FLAG_SIZE - FileOffset;
        DbgPrint("EncryptPostQueryInformation->FileEndOfFileInformation EndOfFile = %d.\n", Info->EndOfFile.QuadPart);
        break;
    }
    default:
    {
        break;
    }
    }

    FltSetCallbackDataDirty(Data);


    if (NULL != StreamContext)
    {
        FltReleaseContext(StreamContext);
        StreamContext = NULL;
    }
    DbgPrint("\n");

    return FLT_POSTOP_FINISHED_PROCESSING;
}


FLT_PREOP_CALLBACK_STATUS
EncryptPreSetInformation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
)
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);


    PEPT_STREAM_CONTEXT StreamContext;
    NTSTATUS Status;

    //������Ȩ��
    Status = EptIsTargetProcess(Data);

    if (!FlagOn(Status, (EPT_PR_ACCESS_READ_WRITE)))
    {
        if (FlagOn(Status, (EPT_PR_ACCESS_NO_ACCESS)))
        {
            return FLT_PREOP_COMPLETE;
        }
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (FlagOn(Data->Flags, FLTFL_CALLBACK_DATA_FAST_IO_OPERATION))
    {
        return FLT_PREOP_DISALLOW_FASTIO;
    }


    if (!EptCreateContext(&StreamContext, FLT_STREAM_CONTEXT)) {

        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (!EptGetOrSetContext(FltObjects->Instance, FltObjects->FileObject, &StreamContext, FLT_STREAM_CONTEXT))
    {
        if (NULL != StreamContext)
        {
            FltReleaseContext(StreamContext);
            StreamContext = NULL;
        }
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (!StreamContext->FlagExist) 
    {
        if (NULL != StreamContext)
        {
            FltReleaseContext(StreamContext);
            StreamContext = NULL;
        }
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }


    DbgPrint("EncryptPreSetInformation->FileInformationClass = %d.\n", Data->Iopb->Parameters.SetFileInformation.FileInformationClass);

    // 4096 -> 3->16      956

    ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);


    PVOID InfoBuffer = Data->Iopb->Parameters.SetFileInformation.InfoBuffer;

    switch (Data->Iopb->Parameters.QueryFileInformation.FileInformationClass)
    {
    
    case FileEndOfFileInformation:
    {
        PFILE_END_OF_FILE_INFORMATION Info = (PFILE_END_OF_FILE_INFORMATION)InfoBuffer;

        StreamContext->FileSize = Info->EndOfFile.QuadPart - FILE_FLAG_SIZE;
        
        if (Info->EndOfFile.QuadPart % AES_BLOCK_SIZE != 0)
        {
            Info->EndOfFile.QuadPart = (Info->EndOfFile.QuadPart / AES_BLOCK_SIZE + 1) * AES_BLOCK_SIZE;
        }

        DbgPrint("EncryptPreSetInformation->FileEndOfFileInformation EndOfFile = %d.\n", Info->EndOfFile.QuadPart);
        
        break;
    }
    case FileAllocationInformation:
    {
        PFILE_ALLOCATION_INFORMATION Info = (PFILE_ALLOCATION_INFORMATION)InfoBuffer;

        if (Info->AllocationSize.QuadPart % AES_BLOCK_SIZE != 0)
        {
            Info->AllocationSize.QuadPart= (Info->AllocationSize.QuadPart / AES_BLOCK_SIZE + 1) * AES_BLOCK_SIZE;
        }

        DbgPrint("EncryptPreSetInformation->FileAllocationInformation Alloc = %d.\n", Info->AllocationSize.QuadPart);
        break;
    }
    case FileStandardInformation:
    {
        PFILE_STANDARD_INFORMATION Info = (PFILE_STANDARD_INFORMATION)InfoBuffer;

        StreamContext->FileSize = Info->EndOfFile.QuadPart - FILE_FLAG_SIZE;

        if (Info->EndOfFile.QuadPart % AES_BLOCK_SIZE != 0)
        {
            Info->EndOfFile.QuadPart = (Info->EndOfFile.QuadPart / AES_BLOCK_SIZE + 1) * AES_BLOCK_SIZE;
        }

        if (Info->AllocationSize.QuadPart % AES_BLOCK_SIZE != 0)
        {
            Info->AllocationSize.QuadPart = (Info->AllocationSize.QuadPart / AES_BLOCK_SIZE + 1) * AES_BLOCK_SIZE;
        }
        
        DbgPrint("EncryptPreSetInformation->FileStandardInformation EndOfFile = %d.\n", Info->EndOfFile.QuadPart);
        DbgPrint("EncryptPreSetInformation->FileStandardInformation Alloc = %d.\n", Info->AllocationSize.QuadPart);
        break;
    }

    }

    ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);

    if (NULL != StreamContext)
    {
        FltReleaseContext(StreamContext);
        StreamContext = NULL;
    }

    return FLT_PREOP_SUCCESS_WITH_CALLBACK;
}

FLT_POSTOP_CALLBACK_STATUS
EncryptPostSetInformation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
)
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);
    UNREFERENCED_PARAMETER(Flags);

    PAGED_CODE();

    return FLT_POSTOP_FINISHED_PROCESSING;
}


FLT_PREOP_CALLBACK_STATUS
EncryptPreCleanUp(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
)
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);

    //NTSTATUS Status;
    PEPT_STREAM_CONTEXT StreamContext;
    NTSTATUS Status;

    //������Ȩ��
    Status = EptIsTargetProcess(Data);

    if (!FlagOn(Status, (EPT_PR_ACCESS_READ_WRITE)))
    {
        if (FlagOn(Status, (EPT_PR_ACCESS_NO_ACCESS)))
        {
            return FLT_PREOP_COMPLETE;
        }
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (!EptCreateContext(&StreamContext, FLT_STREAM_CONTEXT)) {

        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (!EptGetOrSetContext(FltObjects->Instance, FltObjects->FileObject, &StreamContext, FLT_STREAM_CONTEXT))
    {
        if (NULL != StreamContext)
        {
            FltReleaseContext(StreamContext);
            StreamContext = NULL;
        }
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }


    if (!StreamContext->FlagExist) 
    {
        if (NULL != StreamContext)
        {
            FltReleaseContext(StreamContext);
            StreamContext = NULL;
        }
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (NULL != StreamContext)
    {
        FltReleaseContext(StreamContext);
        StreamContext = NULL;
    }

    DbgPrint("EncryptPreCleanUp->EptFileCacheClear hit.\n");
    EptFileCacheClear(FltObjects->FileObject);
    
    return FLT_PREOP_SUCCESS_WITH_CALLBACK;

}


FLT_POSTOP_CALLBACK_STATUS
EncryptPostCleanUp(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
)
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);
    UNREFERENCED_PARAMETER(Flags);

    PAGED_CODE();

    DbgPrint("EncryptPostCleanUp->hit.\n\n");

    return FLT_POSTOP_FINISHED_PROCESSING;
}


FLT_PREOP_CALLBACK_STATUS
EncryptPreClose(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
)
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);

    NTSTATUS Status;
    PEPT_STREAM_CONTEXT StreamContext;

    //������Ȩ��
    Status = EptIsTargetProcess(Data);

    if (!FlagOn(Status, (EPT_PR_ACCESS_READ_WRITE)))
    {
        if (FlagOn(Status, (EPT_PR_ACCESS_NO_ACCESS)))
        {
            return FLT_PREOP_COMPLETE;
        }
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }


    if (!EptCreateContext(&StreamContext, FLT_STREAM_CONTEXT)) {

        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (!EptGetOrSetContext(FltObjects->Instance, FltObjects->FileObject, &StreamContext, FLT_STREAM_CONTEXT))
    {
        if (NULL != StreamContext)
        {
            FltReleaseContext(StreamContext);
            StreamContext = NULL;
        }
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (StreamContext->AppendHeader == EPT_TO_APPEND_ENCRYPT_HEADER)
    {

    }


    //DbgPrint("EncryptPreClose->ready to enter postclose.\n");

    *CompletionContext = StreamContext;

    return FLT_PREOP_SUCCESS_WITH_CALLBACK;
}


FLT_POSTOP_CALLBACK_STATUS
EncryptPostClose(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
)
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);
    UNREFERENCED_PARAMETER(Flags);

    PAGED_CODE();

    PEPT_STREAM_CONTEXT StreamContext;

    StreamContext = CompletionContext;

    if (StreamContext->AppendHeader == EPT_TO_APPEND_ENCRYPT_HEADER)
    {
       
    }

    //DbgPrint("EncryptPostClose->hit.\n");

    if (NULL != StreamContext)
    {
        FltReleaseContext(StreamContext);
        StreamContext = NULL;
    }

    //DbgPrint("\n");

    return FLT_POSTOP_FINISHED_PROCESSING;
}