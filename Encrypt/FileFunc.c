#pragma warning(disable:4996)

#include "filefunc.h"
#include "commport.h"
#include "cryptography.h"


#define FILE_CLEAR_CACHE_USE_ORIGINAL_LOCK  1

//��ȡд�����ͷʱ�ã���Ҳ��̫��ΪɶҪ���¼���ͬ���������������¼����ڶ��߳�ͬ���ķ�ʽ��̫һ�����������ӣ��������......
VOID EptReadWriteCallbackRoutine(IN PFLT_CALLBACK_DATA CallbackData, IN PFLT_CONTEXT Context)
{
    UNREFERENCED_PARAMETER(CallbackData);
    KeSetEvent((PRKEVENT)Context, IO_NO_INCREMENT, FALSE);
}


//����ļ���С��������
ULONG EptGetFileSize(IN PFLT_INSTANCE Instance, IN PFILE_OBJECT FileObject)
{

    FILE_STANDARD_INFORMATION StandardInfo = { 0 };
    ULONG LengthReturned;

    FltQueryInformationFile(Instance, FileObject, &StandardInfo, sizeof(FILE_STANDARD_INFORMATION), FileStandardInformation, &LengthReturned);

    return (ULONG)StandardInfo.EndOfFile.QuadPart;
}


//�����ļ���С��������
NTSTATUS EptSetFileEOF(IN PFLT_INSTANCE Instance, IN PFILE_OBJECT FileObject, LONGLONG FileSize)
{
    NTSTATUS Status;
    FILE_END_OF_FILE_INFORMATION EOF = { 0 };

    EOF.EndOfFile.QuadPart = FileSize;

    Status = FltSetInformationFile(Instance, FileObject, &EOF, sizeof(FILE_END_OF_FILE_INFORMATION), FileEndOfFileInformation);

    if (STATUS_SUCCESS != Status)
    {
        DbgPrint("EptSetFileEOF->FltSetInformationFile failed status = 0x%x.", Status);
        return Status;
    }

    return STATUS_SUCCESS;
}


//��þ��������С
ULONG EptGetVolumeSectorSize(IN PFLT_INSTANCE Instance)
{
    NTSTATUS Status;
    PFLT_VOLUME Volume = { 0 };
    FLT_VOLUME_PROPERTIES VolumeProps = { 0 };
    ULONG LengthReturned = 0;

    Status = FltGetVolumeFromInstance(Instance, &Volume);

    if (!NT_SUCCESS(Status)) {

        DbgPrint("EptGetVolumeSectorSize->FltGetVolumeFromInstance failed. Status = %x\n", Status);
        goto EXIT;
    }

    Status = FltGetVolumeProperties(Volume, &VolumeProps, sizeof(VolumeProps), &LengthReturned);

    if (!NT_SUCCESS(Status))
    {
        //DbgPrint("EptGetVolumeSectorSize->FltGetVolumeProperties failed. Status = %x\n", Status);
        goto EXIT;
    }

EXIT:
    if (NULL != Volume)
    {
        FltObjectDereference(Volume);
        Volume = NULL;
    }

    return VolumeProps.SectorSize;
}


//�ж��Ƿ�Ϊ���м��ܱ�ǵ��ļ�
NTSTATUS EptIsTargetFile(IN PCFLT_RELATED_OBJECTS FltObjects) 
{
    ASSERT(FltObjects != NULL);

	NTSTATUS Status;
	PFLT_VOLUME Volume;
	FLT_VOLUME_PROPERTIES VolumeProps;

    KEVENT Event;

    PVOID ReadBuffer = NULL;
	LARGE_INTEGER ByteOffset = { 0 };
	ULONG Length;


	//����FltReadFile����Length��Ҫ��Length������������С��������
	Status = FltGetVolumeFromInstance(FltObjects->Instance, &Volume);

	if (!NT_SUCCESS(Status)) {

		DbgPrint("EptIsTargetFile->FltGetVolumeFromInstance failed. Status = %x\n", Status);
        goto EXIT;
	}

	Status = FltGetVolumeProperties(Volume, &VolumeProps, sizeof(VolumeProps), &Length);

	if (NT_ERROR(Status)) 
    {
		DbgPrint("EptIsTargetFile->FltGetVolumeProperties failed. Status = %x\n", Status);
        goto EXIT;
	}

	//DbgPrint("VolumeProps.SectorSize = %d.\n", VolumeProps.SectorSize);

	Length = FILE_FLAG_SIZE;
	Length = ROUND_TO_SIZE(Length, VolumeProps.SectorSize);

	//ΪFltReadFile�����ڴ棬֮����Buffer�в���Flag
	ReadBuffer = FltAllocatePoolAlignedWithTag(FltObjects->Instance, NonPagedPool, Length, 'itRB');

	if (!ReadBuffer) 
    {
		DbgPrint("EptIsTargetFile->FltAllocatePoolAlignedWithTag ReadBuffer failed.\n");
        goto EXIT;
	}

	RtlZeroMemory(ReadBuffer, Length);

    KeInitializeEvent(&Event, SynchronizationEvent, FALSE);

    //���ļ����뻺����
    ByteOffset.QuadPart = 0;
    Status = FltReadFile(FltObjects->Instance, FltObjects->FileObject, &ByteOffset, Length, ReadBuffer,
        FLTFL_IO_OPERATION_NON_CACHED | FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET, NULL, EptReadWriteCallbackRoutine, &Event);

    KeWaitForSingleObject(&Event, Executive, KernelMode, TRUE, 0);

   
	if (!NT_SUCCESS(Status)) {

        //STATUS_PENDING
		DbgPrint("EptIsTargetFile->FltReadFile failed. Status = %X.\n", Status);
		
        goto EXIT;

	}

	//DbgPrint("EptIsTargetFile Buffer = %p file content = %s.\n", ReadBuffer, (CHAR*)ReadBuffer);

	if (strncmp(FILE_FLAG, ReadBuffer, strlen(FILE_FLAG)) == 0) 
    {
		DbgPrint("EptIsTargetFile->TargetFile is match.\n");
		Status = EPT_ALREADY_HAVE_ENCRYPT_HEADER;
    }
    else
    {
        Status = EPT_DONT_HAVE_ENCRYPT_HEADER;
    }



EXIT:
    if (NULL != Volume)
    {
        FltObjectDereference(Volume);
        Volume = NULL;
    }

    if (NULL != ReadBuffer)
    {
        FltFreePoolAlignedWithTag(FltObjects->Instance, ReadBuffer, 'itRB');
        ReadBuffer = NULL;
    }
	return Status;
}


//������½����ļ�������д�����ݵ�����д����ܱ��ͷ
NTSTATUS EptWriteEncryptHeader(IN OUT PFLT_CALLBACK_DATA* Data, IN PCFLT_RELATED_OBJECTS FltObjects) {

    ASSERT(Data != NULL);
    ASSERT(FltObjects != NULL);

	NTSTATUS Status;
    FILE_STANDARD_INFORMATION StandardInfo = { 0 };

	PFLT_VOLUME Volume;
	FLT_VOLUME_PROPERTIES VolumeProps;

    KEVENT Event;

	PVOID Buffer;
	LARGE_INTEGER ByteOffset;
	ULONG Length, LengthReturned;


	////��ѯ�ļ���С
	Status = FltQueryInformationFile(FltObjects->Instance, FltObjects->FileObject, &StandardInfo, sizeof(FILE_STANDARD_INFORMATION), FileStandardInformation, &LengthReturned);

	if (!NT_SUCCESS(Status) || Status == STATUS_VOLUME_DISMOUNTED) 
    {
		//DbgPrint("[EptWriteFileHeader]->FltQueryInformationFile failed. Status = %x\n", Status);
		return Status;
	}

    //����FltWriteFile����Length��Ҫ��Length������������С��������
    Status = FltGetVolumeFromInstance(FltObjects->Instance, &Volume);

    if (!NT_SUCCESS(Status)) {

        DbgPrint("[EptWriteFileHeader]->FltGetVolumeFromInstance failed. Status = %x\n", Status);
        return Status;
    }

    Status = FltGetVolumeProperties(Volume, &VolumeProps, sizeof(VolumeProps), &Length);

    if (NULL != Volume)
    {
        FltObjectDereference(Volume);
        Volume = NULL;
    }


	//�����ļ�ͷFILE_FLAG_SIZE��С��д���ļ�flag
	if (StandardInfo.EndOfFile.QuadPart == 0
		&& ((*Data)->Iopb->Parameters.Create.SecurityContext->DesiredAccess & (FILE_WRITE_DATA | FILE_APPEND_DATA))) {

        Length = max(sizeof(FILE_FLAG), FILE_FLAG_SIZE);
        Length = ROUND_TO_SIZE(Length, VolumeProps.SectorSize);

		Buffer = FltAllocatePoolAlignedWithTag(FltObjects->Instance, NonPagedPool, Length, 'wiBF');
		if (!Buffer) {

			DbgPrint("EptWriteEncryptHeader->ExAllocatePoolWithTag Buffer failed.\n");
			return Status;
		}

		RtlZeroMemory(Buffer, Length);

		if (Length >= sizeof(FILE_FLAG))
			RtlMoveMemory(Buffer, FILE_FLAG, sizeof(FILE_FLAG));


        KeInitializeEvent(&Event, SynchronizationEvent, FALSE);

		//д����ܱ��ͷ
		ByteOffset.QuadPart = 0;
		Status = FltWriteFile(FltObjects->Instance, FltObjects->FileObject, &ByteOffset, Length, Buffer,
			FLTFL_IO_OPERATION_NON_CACHED | FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET, NULL, EptReadWriteCallbackRoutine, &Event);

        KeWaitForSingleObject(&Event, Executive, KernelMode, TRUE, 0);


		if (!NT_SUCCESS(Status)) {

			DbgPrint("EptWriteEncryptHeader->NULL FltWriteFile failed. Status = %x\n", Status);
            if (NULL != Buffer)
            {
                FltFreePoolAlignedWithTag(FltObjects->Instance, Buffer, EPT_READ_BUFFER_FLAG);
                Buffer = NULL;
            }
			return Status;
		}

        if (NULL != Buffer)
        {
            FltFreePoolAlignedWithTag(FltObjects->Instance, Buffer, EPT_READ_BUFFER_FLAG);
            Buffer = NULL;
        }

        DbgPrint("EptWriteEncryptHeader->EOF=NULL FltWriteFile success.\n");

        return EPT_WRITE_ENCRYPT_HEADER;

	}
    else if((*Data)->Iopb->Parameters.Create.SecurityContext->DesiredAccess & (FILE_WRITE_DATA | FILE_APPEND_DATA))
    {
        return EPT_TO_APPEND_ENCRYPT_HEADER;
    }

	return EPT_FINISHED_PROCESSING;
}


//�ֶ����ļ�����þ��
NTSTATUS EptCreateFileForHeaderWriting(IN PFLT_INSTANCE Instance, IN PUNICODE_STRING uFileName, OUT HANDLE* phFileHandle)
{

    OBJECT_ATTRIBUTES oaObjectAttributes;
    IO_STATUS_BLOCK ioStatusBlock;

    NTSTATUS Status;


    InitializeObjectAttributes(
        &oaObjectAttributes,
        uFileName,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);


    Status = FltCreateFile(
        gFilterHandle,
        Instance,
        phFileHandle,
        FILE_READ_DATA | FILE_WRITE_DATA,
        &oaObjectAttributes,
        &ioStatusBlock,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        FILE_OPEN_IF,
        FILE_NON_DIRECTORY_FILE | FILE_NO_INTERMEDIATE_BUFFERING,
        NULL,
        0,
        IO_IGNORE_SHARE_ACCESS_CHECK);

    if (!NT_SUCCESS(Status))
    {
        DbgPrint("FileCreateForHeaderWriting->FltCreateFile failed Status = 0x%x.\n", Status);
        return Status;
    }

    return Status;
}


//�ļ�·������תΪDOS�� https://blog.csdn.net/zyorz/category_6871818.html
NTSTATUS EptQuerySymbolicLink(IN PUNICODE_STRING SymbolicLinkName, OUT PUNICODE_STRING LinkTarget)
//����\\??\\c:-->\\device\\\harddiskvolume1
//LinkTarget.Bufferע��Ҫ�ͷ�
{
    OBJECT_ATTRIBUTES   oa = { 0 };
    NTSTATUS            status = 0;
    HANDLE              handle = NULL;

    InitializeObjectAttributes(
        &oa,
        SymbolicLinkName,
        OBJ_CASE_INSENSITIVE,
        0,
        0);

    status = ZwOpenSymbolicLinkObject(&handle, GENERIC_READ, &oa);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    LinkTarget->MaximumLength = 260 * sizeof(WCHAR);
    LinkTarget->Length = 0;
    LinkTarget->Buffer = ExAllocatePoolWithTag(PagedPool, LinkTarget->MaximumLength, 'SOD');
    if (!LinkTarget->Buffer)
    {
        ZwClose(handle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(LinkTarget->Buffer, LinkTarget->MaximumLength);

    status = ZwQuerySymbolicLinkObject(handle, LinkTarget, NULL);
    ZwClose(handle);

    if (!NT_SUCCESS(status))
    {
        ExFreePool(LinkTarget->Buffer);
    }

    return status;
}


//�õ���Ӧ���ʵ�� https://blog.csdn.net/feixi7358/article/details/85039734
PFLT_INSTANCE EptGetVolumeInstance(IN PFLT_FILTER pFilter, IN PUNICODE_STRING pVolumeName)
////����ļ������̵�ʵ��
//PFLT_INSTANCE fileInstance = NULL;
//UNICODE_STRING  pVolumeNamec;
//RtlInitUnicodeString(&pVolumeNamec, L"\\Device\\HarddiskVolume2");//���ڵľ�
//fileInstance = XBFltGetVolumeInstance(gFilterHandle, &pVolumeNamec);
{
    NTSTATUS		status;
    PFLT_INSTANCE	pInstance = NULL;
    PFLT_VOLUME		pVolumeList[100];
    ULONG			uRet;
    UNICODE_STRING	uniName = { 0 };
    ULONG 			index = 0;
    WCHAR			wszNameBuffer[260] = { 0 };

    status = FltEnumerateVolumes(pFilter,
        NULL,
        0,
        &uRet);
    if (status != STATUS_BUFFER_TOO_SMALL)
    {
        return NULL;
    }

    status = FltEnumerateVolumes(pFilter,
        pVolumeList,
        uRet,
        &uRet);

    if (!NT_SUCCESS(status))
    {

        return NULL;
    }
    uniName.Buffer = wszNameBuffer;

    if (uniName.Buffer == NULL)
    {
        for (index = 0; index < uRet; index++)
            FltObjectDereference(pVolumeList[index]);

        return NULL;
    }

    uniName.MaximumLength = sizeof(wszNameBuffer);

    for (index = 0; index < uRet; index++)
    {
        uniName.Length = 0;

        status = FltGetVolumeName(pVolumeList[index],
            &uniName,
            NULL);

        if (!NT_SUCCESS(status))
            continue;

        if (RtlCompareUnicodeString(&uniName,
            pVolumeName,
            TRUE) != 0)
            continue;

        status = FltGetVolumeInstanceFromName(pFilter,
            pVolumeList[index],
            NULL,
            &pInstance);

        if (NT_SUCCESS(status))
        {
            FltObjectDereference(pInstance);
            break;
        }
    }

    for (index = 0; index < uRet; index++)
        FltObjectDereference(pVolumeList[index]);
    return pInstance;
}


//��Ȩ���������Ӧ��ִ�к����������ȥ����ͷ���������ݣ��޸�StreamContext
NTSTATUS EptRemoveEncryptHeaderAndDecrypt(IN PWCHAR FileName)
{

    if (NULL == FileName)
    {
        DbgPrint("EptRemoveEncryptHeaderAndDecrypt->FileName is NULL.\n");
        return EPT_NULL_POINTER;
    }

    NTSTATUS Status;

    UNICODE_STRING uFileName = { 0 };
    HANDLE hFile = NULL;
    PFILE_OBJECT FileObject = { 0 };

    PWCHAR lpFileName = FileName;
    WCHAR wSymbolLinkName[260] = { 0 };
    UNICODE_STRING uSymbolLinkName = { 0 };
    UNICODE_STRING uDosName = { 0 };

    PFLT_INSTANCE Instance = { 0 };
    ULONG FileSize = 0, SectorSize = 0, ReadLength = 0;

    PCHAR ReadBuffer = NULL, EncryptHeader = NULL;
    LARGE_INTEGER ByteOffset;

    PEPT_STREAM_CONTEXT StreamContext = NULL;

    RtlInitUnicodeString(&uFileName, FileName);

    //DbgPrint("EptRemoveEncryptHeaderAndDecrypt->Test FileName = %wZ.\n", uFileName);

    //���ļ������hFile���õ�FileObject����FltReadFile/FltWriteFile��
    Status = EptCreateFileForHeaderWriting(NULL, &uFileName, &hFile);

    if (!NT_SUCCESS(Status))
    {
        DbgPrint("EptRemoveEncryptHeaderAndDecrypt->FileCreateForHeaderWriting failed ststus = 0x%x.\n", Status);
        goto EXIT;
    }

    Status = ObReferenceObjectByHandle(hFile, STANDARD_RIGHTS_ALL, *IoFileObjectType, KernelMode, (PVOID*)&FileObject, NULL);

    if (STATUS_SUCCESS != Status)
    {
        DbgPrint("EptRemoveEncryptHeaderAndDecrypt->ObReferenceObjectByHandle failed ststus = 0x%x.\n", Status);
        goto EXIT;
    }


    //���ļ��ķ������ӣ��ҵ���Ӧ���̵�DOS�����ҵ����̵�Instance
    while (*lpFileName != L':')
    {
        lpFileName++;
    }

    //wSymbolLinkName = L"\\??\\C:"
    RtlMoveMemory(wSymbolLinkName, FileName, (lpFileName - FileName + 1) * sizeof(WCHAR));

    RtlInitUnicodeString(&uSymbolLinkName, wSymbolLinkName);

    Status = EptQuerySymbolicLink(&uSymbolLinkName, &uDosName);

    if (STATUS_SUCCESS != Status)
    {
        DbgPrint("EptRemoveEncryptHeaderAndDecrypt->EptQuerySymbolicLink failed ststus = 0x%x.\n", Status);
        goto EXIT;
    }

    Instance = EptGetVolumeInstance(gFilterHandle, &uDosName);

    if (NULL == Instance)
    {
        DbgPrint("EptRemoveEncryptHeaderAndDecrypt->EptGetVolumeInstance failed.\n");
        goto EXIT;
    }


    //�ж��Ƿ��м���ͷ
    EncryptHeader = FltAllocatePoolAlignedWithTag(Instance, NonPagedPool, FILE_FLAG_SIZE, EPT_READ_BUFFER_FLAG);

    if (NULL == EncryptHeader)
    {
        DbgPrint("EptRemoveEncryptHeaderAndDecrypt->FltAllocatePoolAlignedWithTag EncryptHeader failed.\n");
        goto EXIT;
    }

    RtlZeroMemory(EncryptHeader, FILE_FLAG_SIZE);

    ByteOffset.QuadPart = 0;
    Status = FltReadFile(Instance, FileObject, &ByteOffset, (ULONG)FILE_FLAG_SIZE, (PVOID)EncryptHeader,
        FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET | FLTFL_IO_OPERATION_NON_CACHED, NULL, NULL, NULL);

    if (!NT_SUCCESS(Status))
    {
        //STATUS_SUCCESS
        //STATUS_END_OF_FILE
        DbgPrint("EptRemoveEncryptHeaderAndDecrypt->FltReadFile read encryptheader failed. Status = %X.\n", Status);
        Status = EPT_DONT_HAVE_ENCRYPT_HEADER;
        goto EXIT;
    }

    if (strncmp(EncryptHeader, FILE_FLAG, strlen(FILE_FLAG)) != 0)
    {
        DbgPrint("EptRemoveEncryptHeaderAndDecrypt->%ws is an unencrypted file.\n", FileName);
        Status = EPT_DONT_HAVE_ENCRYPT_HEADER;
        goto EXIT;
    }


    //������ļ���С�Ǽ���ͷ0x1000+���Ķ����Ĵ�С
    FileSize = EptGetFileSize(Instance, FileObject);
    SectorSize = EptGetVolumeSectorSize(Instance);

    ReadLength = ROUND_TO_SIZE(FileSize - FILE_FLAG_SIZE, SectorSize);

    ReadBuffer = FltAllocatePoolAlignedWithTag(Instance, NonPagedPool, ReadLength, EPT_READ_BUFFER_FLAG);

    if (NULL == ReadBuffer)
    {
        DbgPrint("EptRemoveEncryptHeaderAndDecrypt->FltAllocatePoolAlignedWithTag ReadBuffer failed.\n");
        goto EXIT;
    }

    RtlZeroMemory(ReadBuffer, ReadLength);

    //�����Ķ��뻺����
    ByteOffset.QuadPart = FILE_FLAG_SIZE;      //ȥ������ͷ
    Status = FltReadFile(Instance, FileObject, &ByteOffset, (ULONG)ReadLength, (PVOID)ReadBuffer,
        FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET | FLTFL_IO_OPERATION_NON_CACHED, NULL, NULL, NULL);

    if (!NT_SUCCESS(Status))
    {
        DbgPrint("EptRemoveEncryptHeaderAndDecrypt->FltReadFile failed. Status = %X.\n", Status);
        goto EXIT;
    }

    //����
    Status = EptAesDecrypt((PUCHAR)ReadBuffer, FileSize - FILE_FLAG_SIZE);

    if (STATUS_SUCCESS != Status)
    {
        DbgPrint("EptRemoveEncryptHeaderAndDecrypt->EptAesDecrypt failed. Status = %X.\n", Status);
        goto EXIT;
    }

    //����ȥ�������ļ�ͷ�Ĵ�С
    Status = EptSetFileEOF(Instance, FileObject, strlen(ReadBuffer));

    if (STATUS_SUCCESS != Status)
    {
        DbgPrint("EptRemoveEncryptHeaderAndDecrypt->EptSetFileEOF failed. Status = %X.\n", Status);
        goto EXIT;
    }

    //д��ԭʼ����
    ByteOffset.QuadPart = 0;
    Status = FltWriteFile(Instance, FileObject, &ByteOffset, (ULONG)strlen(ReadBuffer), ReadBuffer,
        FLTFL_IO_OPERATION_NON_CACHED | FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET, NULL, NULL, NULL);

    if (!NT_SUCCESS(Status))
    {
        //д��ʧ�ܣ��ָ�EOF
        EptSetFileEOF(Instance, FileObject, FileSize);
        DbgPrint("EptRemoveEncryptHeaderAndDecrypt->FltWriteFile failed. Status = %X.\n", Status);
        goto EXIT;
    }

    if (!EptCreateContext(&StreamContext, FLT_STREAM_CONTEXT)) 
    {
        EptSetFileEOF(Instance, FileObject, FileSize);
        DbgPrint("EptRemoveEncryptHeaderAndDecrypt->EptCreateContext failed.");
        goto EXIT;
    }

    if (!EptGetOrSetContext(Instance, FileObject, &StreamContext, FLT_STREAM_CONTEXT))
    {
        EptSetFileEOF(Instance, FileObject, FileSize);
        DbgPrint("EptRemoveEncryptHeaderAndDecrypt->EptGetOrSetContext failed.");
        goto EXIT;
    }

    ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);
    StreamContext->FlagExist = 0;
    ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);

    //һ��ҪFlushCache
    EptFileCacheClear(FileObject);

    DbgPrint("EptRemoveEncryptHeaderAndDecrypt->success origfile = %s\n", ReadBuffer);

    Status = EPT_REMOVE_ENCRYPT_HEADER;

EXIT:
    if (NULL != FileObject)
    {
        ObDereferenceObject(FileObject);
        FileObject = NULL;
    }

    if (NULL != hFile)
    {
        FltClose(hFile);
        hFile = NULL;
    }

    if (NULL != uDosName.Buffer)
    {
        ExFreePool(uDosName.Buffer);
        uDosName.Buffer = NULL;
    }

    if (NULL != ReadBuffer)
    {
        FltFreePoolAlignedWithTag(Instance, ReadBuffer, EPT_READ_BUFFER_FLAG);
        ReadBuffer = NULL;
    }

    if (NULL != EncryptHeader)
    {
        FltFreePoolAlignedWithTag(Instance, EncryptHeader, EPT_READ_BUFFER_FLAG);
        EncryptHeader = NULL;
    }

    if (NULL != StreamContext)
    {
        FltReleaseContext(StreamContext);
        StreamContext = NULL;
    }

    return Status;
}


//��Ȩ���������Ŀ���ļ����ϻ���ͷ���������ݣ��޸�StreamContext
NTSTATUS EptAppendEncryptHeaderAndEncryptEx(IN PWCHAR FileName)
{

    if (NULL == FileName)
    {
        DbgPrint("EptAppendEncryptHeaderAndEncryptEx->FileName is NULL.\n");
        return EPT_NULL_POINTER;
    }

    NTSTATUS Status;

    UNICODE_STRING uFileName = { 0 };
    HANDLE hFile = NULL;
    PFILE_OBJECT FileObject = { 0 };

    PWCHAR lpFileName = FileName;
    WCHAR wSymbolLinkName[260] = { 0 };
    UNICODE_STRING uSymbolLinkName = { 0 };
    UNICODE_STRING uDosName = { 0 };

    PFLT_INSTANCE Instance = { 0 };
    ULONG FileSize = 0, SectorSize = 0, ReadLength = 0, EncryptedLength = 0;

    PCHAR ReadBuffer = NULL, EncryptedBuffer = NULL;
    LARGE_INTEGER ByteOffset;

    PEPT_STREAM_CONTEXT StreamContext = NULL;

    RtlInitUnicodeString(&uFileName, FileName);

    //DbgPrint("EptRemoveEncryptHeaderAndDecrypt->Test FileName = %wZ.\n", uFileName);

    //���ļ������hFile���õ�FileObject����FltReadFile/FltWriteFile��
    //L"\\??\\C:\\Desktop\\a.txt"
    Status = EptCreateFileForHeaderWriting(NULL, &uFileName, &hFile);

    if (!NT_SUCCESS(Status))
    {
        DbgPrint("EptAppendEncryptHeaderAndEncryptEx->FileCreateForHeaderWriting failed ststus = 0x%x.\n", Status);
        goto EXIT;
    }

    Status = ObReferenceObjectByHandle(hFile, STANDARD_RIGHTS_ALL, *IoFileObjectType, KernelMode, (PVOID*)&FileObject, NULL);

    if (STATUS_SUCCESS != Status)
    {
        DbgPrint("EptAppendEncryptHeaderAndEncryptEx->ObReferenceObjectByHandle failed ststus = 0x%x.\n", Status);
        goto EXIT;
    }


    //���ļ��ķ������ӣ��ҵ���Ӧ���̵�DOS�����ҵ����̵�Instance
    while (*lpFileName != L':')
    {
        lpFileName++;
    }

    //wSymbolLinkName = L"\\??\\C:"
    RtlMoveMemory(wSymbolLinkName, FileName, (lpFileName - FileName + 1) * sizeof(WCHAR));

    RtlInitUnicodeString(&uSymbolLinkName, wSymbolLinkName);

    Status = EptQuerySymbolicLink(&uSymbolLinkName, &uDosName);

    if (STATUS_SUCCESS != Status)
    {
        DbgPrint("EptAppendEncryptHeaderAndEncryptEx->EptQuerySymbolicLink failed ststus = 0x%x.\n", Status);
        goto EXIT;
    }

    Instance = EptGetVolumeInstance(gFilterHandle, &uDosName);

    if (NULL == Instance)
    {
        DbgPrint("EptAppendEncryptHeaderAndEncryptEx->EptGetVolumeInstance failed.\n");
        goto EXIT;
    }

    FileSize = EptGetFileSize(Instance, FileObject);
    SectorSize = EptGetVolumeSectorSize(Instance);

    ReadLength = ROUND_TO_SIZE(FileSize, SectorSize);

    //���ڿ��ļ���Ҫ�����⴦������ReadBuffer��������
    if (0 == ReadLength)
    {
        ReadLength += 16;
    }

    ReadBuffer = FltAllocatePoolAlignedWithTag(Instance, NonPagedPool, ReadLength, EPT_READ_BUFFER_FLAG);

    if (NULL == ReadBuffer)
    {
        DbgPrint("EptAppendEncryptHeaderAndEncryptEx->FltAllocatePoolAlignedWithTag ReadBuffer failed.\n");
        goto EXIT;
    }

    RtlZeroMemory(ReadBuffer, ReadLength);

    if (16 == ReadLength && FileSize == 0)
    {
        ReadLength -= 16;
    }

    //���ļ����뻺����
    ByteOffset.QuadPart = 0;
    Status = FltReadFile(Instance, FileObject, &ByteOffset, (ULONG)ReadLength, (PVOID)ReadBuffer,
        FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET | FLTFL_IO_OPERATION_NON_CACHED, NULL, NULL, NULL);

    if (!NT_SUCCESS(Status))
    {   
        //STATUS_SUCCESS
        DbgPrint("EptAppendEncryptHeaderAndEncryptEx->FltReadFile failed. Status = %X.\n", Status);
        goto EXIT;
    }

    if (strncmp(FILE_FLAG, ReadBuffer, strlen(FILE_FLAG)) == 0)
    {
        DbgPrint("EptAppendEncryptHeaderAndEncryptEx->File has been already encrypted.\n");
        Status = EPT_ALREADY_HAVE_ENCRYPT_HEADER;
        goto EXIT;
    }


    //��ȡ���ܺ����ݵĴ�С
    if (FileSize > 0)
    {
        if (!EptAesEncrypt((PUCHAR)ReadBuffer, &EncryptedLength, TRUE))
        {
            DbgPrint("EptAppendEncryptHeaderAndEncryptEx->EptAesEncrypt count size failed.\n");
            goto EXIT;
        }
    }

    EncryptedBuffer = FltAllocatePoolAlignedWithTag(Instance, NonPagedPool, (LONGLONG)EncryptedLength + FILE_FLAG_SIZE, EPT_READ_BUFFER_FLAG);

    if (NULL == EncryptedBuffer)
    {
        DbgPrint("EptAppendEncryptHeaderAndEncryptEx->FltAllocatePoolAlignedWithTag EncryptedBuffer failed.\n");
        goto EXIT;
    }

    RtlZeroMemory(EncryptedBuffer, (LONGLONG)EncryptedLength + FILE_FLAG_SIZE);

    RtlMoveMemory(EncryptedBuffer + FILE_FLAG_SIZE, ReadBuffer, strlen(ReadBuffer));


    if (FileSize > 0)
    {
        if (!EptAesEncrypt((PUCHAR)EncryptedBuffer + FILE_FLAG_SIZE, &EncryptedLength, FALSE))
        {
            DbgPrint("EptAppendEncryptHeaderAndEncryptEx->EptAesEncrypt encrypte buffer failed.\n");
            goto EXIT;
        }
    }
    

    RtlMoveMemory(EncryptedBuffer, FILE_FLAG, strlen(FILE_FLAG));
    
    DbgPrint("EncryptedLength = %d FileSize = %d.\n", EncryptedLength, FileSize);

    //���ü��ϼ����ļ�ͷ���ļ���С
    Status = EptSetFileEOF(Instance, FileObject, (LONGLONG)EncryptedLength + FILE_FLAG_SIZE);

    if (STATUS_SUCCESS != Status)
    {
        DbgPrint("EptAppendEncryptHeaderAndEncryptEx->EptSetFileEOF failed. Status = %X.\n", Status);
        goto EXIT;
    }

    //д������ļ�ͷ������
    ByteOffset.QuadPart = 0;
    Status = FltWriteFile(Instance, FileObject, &ByteOffset, (ULONG)EncryptedLength + FILE_FLAG_SIZE, EncryptedBuffer,
        FLTFL_IO_OPERATION_NON_CACHED | FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET, NULL, NULL, NULL);

    if (!NT_SUCCESS(Status))
    {
        //д��ʧ�ܣ��ָ�EOF
        EptSetFileEOF(Instance, FileObject, FileSize);
        DbgPrint("EptAppendEncryptHeaderAndEncryptEx->FltWriteFile failed. Status = %X.\n", Status);
        goto EXIT;
    }

    if (!EptCreateContext(&StreamContext, FLT_STREAM_CONTEXT))
    {
        EptSetFileEOF(Instance, FileObject, FileSize);
        DbgPrint("EptAppendEncryptHeaderAndEncryptEx->EptCreateContext failed.");
        goto EXIT;
    }

    if (!EptGetOrSetContext(Instance, FileObject, &StreamContext, FLT_STREAM_CONTEXT))
    {
        EptSetFileEOF(Instance, FileObject, FileSize);
        DbgPrint("EptAppendEncryptHeaderAndEncryptEx->EptGetOrSetContext failed.");
        goto EXIT;
    }

    ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);
    StreamContext->FlagExist = EPT_ENCRYPT_FLAG_EXIST;
    StreamContext->FileSize = FileSize;
    StreamContext->AppendHeader = EPT_APPEND_ENCRYPT_HEADER;
    ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);

    //һ��ҪFlushCache
    EptFileCacheClear(FileObject);

    DbgPrint("EptAppendEncryptHeaderAndEncryptEx->success origfile = %s\n", ReadBuffer);

    Status = EPT_APPEND_ENCRYPT_HEADER;

EXIT:
    if (NULL != FileObject)
    {
        ObDereferenceObject(FileObject);
        FileObject = NULL;
    }

    if (NULL != hFile)
    {
        FltClose(hFile);
        hFile = NULL;
    }

    if (NULL != uDosName.Buffer)
    {
        ExFreePool(uDosName.Buffer);
        uDosName.Buffer = NULL;
    }

    if (NULL != ReadBuffer)
    {
        FltFreePoolAlignedWithTag(Instance, ReadBuffer, EPT_READ_BUFFER_FLAG);
        ReadBuffer = NULL;
    }

    if (NULL != EncryptedBuffer)
    {
        FltFreePoolAlignedWithTag(Instance, EncryptedBuffer, EPT_READ_BUFFER_FLAG);
        EncryptedBuffer = NULL;
    }

    if (NULL != StreamContext)
    {
        FltReleaseContext(StreamContext);
        StreamContext = NULL;
    }

    return Status;
}


//����ļ����壬https://github.com/SchineCompton/Antinvader
VOID EptFileCacheClear(IN PFILE_OBJECT pFileObject)
{
    // FCB
    PFSRTL_COMMON_FCB_HEADER pFcb;

    // ˯��ʱ�� ����KeDelayExecutionThread
    LARGE_INTEGER liInterval;

    // �Ƿ���Ҫ�ͷ���Դ
    BOOLEAN bNeedReleaseResource = FALSE;

    // �Ƿ���Ҫ�ͷŷ�ҳ��Դ
    BOOLEAN bNeedReleasePagingIoResource = FALSE;

    // IRQL
    KIRQL irql;

    // ѭ��ʱ�Ƿ�����
    BOOLEAN bBreak = TRUE;

    // �Ƿ���Դ������
    BOOLEAN bLockedResource = FALSE;

    // �Ƿ��Ƿ�ҳ��Դ������
    BOOLEAN bLockedPagingIoResource = FALSE;

    // Resource �� PagingIoResource ��Դ�������Ⱥ�˳��
    BOOLEAN isPagingIoResourceLockedFirst = FALSE;

    //
    // ��ȡFCB
    //
    pFcb = (PFSRTL_COMMON_FCB_HEADER)pFileObject->FsContext;

    //
    // ���û��FCB ֱ�ӷ���
    //
    if (pFcb == NULL) {
        /*
        #ifdef DBG
                __asm int 3
        #endif*/
        return;
    }

    //
    // ��֤��ǰIRQL <= APC_LEVEL
    //

    irql = KeGetCurrentIrql();

    if (irql > APC_LEVEL) {
#if defined(DBG) && !defined(_WIN64)
        __asm int 3
#endif
        return;
    }

    //
    // ����˯��ʱ��
    //
    liInterval.QuadPart = -1 * (LONGLONG)50;

    //
    // �����ļ�ϵͳ
    //
    FsRtlEnterFileSystem();

    isPagingIoResourceLockedFirst = FALSE;

    //
    // FILE_CLEAR_CACHE_USE_ORIGINAL_LOCK, ע��: �ú궨���� AntinvaderDef.h ͷ�ļ���
    //
#if defined(FILE_CLEAR_CACHE_USE_ORIGINAL_LOCK) && (FILE_CLEAR_CACHE_USE_ORIGINAL_LOCK != 0)
    //
    // ѭ������, һ��Ҫ����, �������������.
    //
    for (;;) {
        //
        // ��ʼ������
        //
        bBreak = TRUE;
        bNeedReleaseResource = FALSE;
        bNeedReleasePagingIoResource = FALSE;
        bLockedResource = FALSE;
        bLockedPagingIoResource = FALSE;

        //
        // ��FCB������
        //
        if (pFcb->PagingIoResource) {
            if (bLockedPagingIoResource == FALSE) {
                bLockedPagingIoResource = ExIsResourceAcquiredExclusiveLite(pFcb->PagingIoResource);
                if (bLockedPagingIoResource) {
                    if (bLockedResource == FALSE)
                        isPagingIoResourceLockedFirst = TRUE;
                    bNeedReleasePagingIoResource = TRUE;
                }
            }
        }

        //
        // ʹ����, ������, һ����.....
        //
        if (pFcb->Resource) {
            if (bLockedResource == FALSE) {
                //
                // �ȳ�����һ����Դ
                //
                if (ExIsResourceAcquiredExclusiveLite(pFcb->Resource) == FALSE) {
                    //
                    // û�õ���Դ, ����һ��.
                    //
                    if (bLockedPagingIoResource) {
                        if (ExAcquireResourceExclusiveLite(pFcb->Resource, FALSE) == FALSE) {
                            bBreak = FALSE;
                            bLockedResource = FALSE;
                            bNeedReleaseResource = FALSE;
                        }
                        else {
                            bLockedResource = TRUE;
                            bNeedReleaseResource = TRUE;
                        }
                    }
                    else {
                        if (bLockedResource == FALSE) {
                            ExAcquireResourceExclusiveLite(pFcb->Resource, TRUE);
                            bLockedResource = TRUE;
                            bNeedReleaseResource = TRUE;
                            isPagingIoResourceLockedFirst = FALSE;
                        }
                    }
                }
                else {
                    bLockedResource = TRUE;
                    bNeedReleaseResource = TRUE;
                }
            }
        }

        if (pFcb->PagingIoResource) {
            if (bLockedPagingIoResource == FALSE) {
                //
                // ������ PagingIoResource ����Դ
                //
                if (bLockedResource) {
                    if (ExAcquireResourceExclusiveLite(pFcb->PagingIoResource, FALSE) == FALSE) {
                        bBreak = FALSE;
                        bLockedPagingIoResource = FALSE;
                        bNeedReleasePagingIoResource = FALSE;
                    }
                    else {
                        if (bLockedResource == FALSE)
                            isPagingIoResourceLockedFirst = TRUE;
                        bLockedPagingIoResource = TRUE;
                        bNeedReleasePagingIoResource = TRUE;
                    }
                }
                else {
                    if (bLockedPagingIoResource == FALSE) {
                        ExAcquireResourceExclusiveLite(pFcb->PagingIoResource, TRUE);
                        if (bLockedResource == FALSE)
                            isPagingIoResourceLockedFirst = TRUE;
                        bLockedPagingIoResource = TRUE;
                        bNeedReleasePagingIoResource = TRUE;
                    }
                }
            }
        }

        if (bLockedResource && bLockedPagingIoResource) {
            break;
        }

        if (bBreak) {
            break;
        }

#if defined(FILE_CLEAR_CACHE_USE_ORIGINAL_LOCK) && (FILE_CLEAR_CACHE_USE_ORIGINAL_LOCK != 0)
        if (isPagingIoResourceLockedFirst) {
            if (bNeedReleasePagingIoResource) {
                if (pFcb->PagingIoResource)
                    ExReleaseResourceLite(pFcb->PagingIoResource);
                bLockedPagingIoResource = FALSE;
                bNeedReleasePagingIoResource = FALSE;
            }
            if (bNeedReleaseResource) {
                if (pFcb->Resource)
                    ExReleaseResourceLite(pFcb->Resource);
                bLockedResource = TRUE;
                bNeedReleaseResource = TRUE;
            }
        }
        else {
            if (bNeedReleaseResource) {
                if (pFcb->Resource)
                    ExReleaseResourceLite(pFcb->Resource);
                bLockedResource = TRUE;
                bNeedReleaseResource = TRUE;
            }
            if (bNeedReleasePagingIoResource) {
                if (pFcb->PagingIoResource)
                    ExReleaseResourceLite(pFcb->PagingIoResource);
                bLockedPagingIoResource = FALSE;
                bNeedReleasePagingIoResource = FALSE;
            }
        }
        isPagingIoResourceLockedFirst = FALSE;
#endif

        /*
        if (irql == PASSIVE_LEVEL) {
//          FsRtlExitFileSystem();
            KeDelayExecutionThread(KernelMode, FALSE, &liInterval);
        }
        else {
            KEVENT waitEvent;
            KeInitializeEvent(&waitEvent, NotificationEvent, FALSE);
            KeWaitForSingleObject(&waitEvent, Executive, KernelMode, FALSE, &liInterval);
        }
        */
    }

#else // !FILE_CLEAR_CACHE_USE_ORIGINAL_LOCK

    if (pFcb->PagingIoResource) {
        ExAcquireResourceExclusiveLite(pFcb->PagingIoResource, TRUE);
        bLockedPagingIoResource = TRUE;
    }

#endif // FILE_CLEAR_CACHE_USE_ORIGINAL_LOCK

    //
    // �����õ�����
    //
    if (pFileObject->SectionObjectPointer) {
        IO_STATUS_BLOCK ioStatus;
        IoSetTopLevelIrp((PIRP)FSRTL_FSP_TOP_LEVEL_IRP);
        CcFlushCache(pFileObject->SectionObjectPointer, NULL, 0, &ioStatus);

        if (pFileObject->SectionObjectPointer->ImageSectionObject) {
            MmFlushImageSection(pFileObject->SectionObjectPointer, MmFlushForWrite); // MmFlushForDelete()
        }

        CcPurgeCacheSection(pFileObject->SectionObjectPointer, NULL, 0, FALSE);
        IoSetTopLevelIrp(NULL);
    }

#if defined(FILE_CLEAR_CACHE_USE_ORIGINAL_LOCK) && (FILE_CLEAR_CACHE_USE_ORIGINAL_LOCK != 0)
    if (isPagingIoResourceLockedFirst) {
        if (bNeedReleasePagingIoResource) {
            if (pFcb->PagingIoResource)
                ExReleaseResourceLite(pFcb->PagingIoResource);
        }
        if (bNeedReleaseResource) {
            if (pFcb->Resource)
                ExReleaseResourceLite(pFcb->Resource);
        }
    }
    else {
        if (bNeedReleaseResource) {
            if (pFcb->Resource)
                ExReleaseResourceLite(pFcb->Resource);
        }
        if (bNeedReleasePagingIoResource) {
            if (pFcb->PagingIoResource)
                ExReleaseResourceLite(pFcb->PagingIoResource);
        }
    }
#else // !FILE_CLEAR_CACHE_USE_ORIGINAL_LOCK
    if (bLockedPagingIoResource == TRUE) {
        if (pFcb->PagingIoResource != NULL) {
            ExReleaseResourceLite(pFcb->PagingIoResource);
        }
        bLockedPagingIoResource = FALSE;
    }
#endif // FILE_CLEAR_CACHE_USE_ORIGINAL_LOCK

    FsRtlExitFileSystem();
    /*
    Acquire:
        FsRtlEnterFileSystem();

        if (Fcb->Resource)
            ResourceAcquired = ExAcquireResourceExclusiveLite(Fcb->Resource, TRUE);
        if (Fcb->PagingIoResource)
            PagingIoResourceAcquired = ExAcquireResourceExclusive(Fcb->PagingIoResource, FALSE);
        else
            PagingIoResourceAcquired = TRUE ;
        if (!PagingIoResourceAcquired) {
            if (Fcb->Resource)  ExReleaseResource(Fcb->Resource);
            FsRtlExitFileSystem();
            KeDelayExecutionThread(KernelMode,FALSE,&Delay50Milliseconds);
            goto Acquire;
        }

        if (FileObject->SectionObjectPointer) {
            IoSetTopLevelIrp( (PIRP)FSRTL_FSP_TOP_LEVEL_IRP);

            if (bIsFlushCache) {
                CcFlushCache( FileObject->SectionObjectPointer, FileOffset, Length, &IoStatus);
            }

            if (FileObject->SectionObjectPointer->ImageSectionObject) {
                MmFlushImageSection(
                    FileObject->SectionObjectPointer,
                    MmFlushForWrite);
            }

            if (FileObject->SectionObjectPointer->DataSectionObject) {
                PurgeRes = CcPurgeCacheSection(FileObject->SectionObjectPointer,
                    NULL,
                    0,
                    FALSE);
            }

            IoSetTopLevelIrp(NULL);
        }

        if (Fcb->PagingIoResource)
            ExReleaseResourceLite(Fcb->PagingIoResource);

        if (Fcb->Resource)
            ExReleaseResourceLite(Fcb->Resource);

        FsRtlExitFileSystem();
        */
}
