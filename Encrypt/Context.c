

#include "context.h"


VOID EptSetFlagInContext(IN LONG* Flag, IN BOOLEAN Set) {

	if (NULL == Flag)
	{
		DbgPrint("[EptSetFlagInContext]->Flag is NULL.\n");
		return;
	}

	if (Set) {

		InterlockedIncrement(Flag);
	}

}


BOOLEAN EptCreateContext(IN OUT PFLT_CONTEXT* CompletionContext, IN FLT_CONTEXT_TYPE ContextType) {

	NTSTATUS Status;
	PFLT_CONTEXT Context = NULL;

	switch (ContextType) {

	case FLT_STREAM_CONTEXT:
	{
		PEPT_STREAM_CONTEXT StreamContext = NULL;

		Status = FltAllocateContext(gFilterHandle, FLT_STREAM_CONTEXT, sizeof(EPT_STREAM_CONTEXT), NonPagedPool, &StreamContext);

		if (!NT_SUCCESS(Status)) {

			DbgPrint("EptCreateContext->FltAllocateContext FLT_STREAM_CONTEXT failed. Status = %X\n", Status);
			return FALSE;
		}

		RtlZeroMemory(StreamContext, sizeof(EPT_STREAM_CONTEXT));

		StreamContext->Resource = ExAllocatePoolWithTag(NonPagedPool, sizeof(ERESOURCE), STREAM_CONTEXT_TAG);

		if (NULL == StreamContext->Resource)
		{
			DbgPrint("EptCreateContext->StreamContext->Resource ExAllocatePoolWithTag failed.\n");
			FltReleaseContext(StreamContext);
			return FALSE;
		}

		ExInitializeResourceLite(StreamContext->Resource);

		StreamContext->FileName.Buffer = ExAllocatePoolWithTag(NonPagedPool, 260 * sizeof(WCHAR), STREAM_CONTEXT_TAG);

		if (NULL == StreamContext->FileName.Buffer)
		{
			DbgPrint("EptCreateContext->StreamContext->FileName.Buffer ExAllocatePoolWithTag failed.\n");
			FltReleaseContext(StreamContext);
			return FALSE;
		}

		RtlZeroMemory(StreamContext->FileName.Buffer, 260 * sizeof(WCHAR));
		StreamContext->FileName.Length = 260;
		StreamContext->FileName.MaximumLength = 260;

		*CompletionContext = StreamContext;

		return TRUE;

	}
	case FLT_STREAMHANDLE_CONTEXT:
	{
		Status = FltAllocateContext(gFilterHandle, FLT_STREAMHANDLE_CONTEXT, sizeof(EPT_STREAMHANDLE_CONTEXT), NonPagedPool, &Context);

		if (!NT_SUCCESS(Status)) {

			DbgPrint("[EptCreateContext]->FltAllocateContext FLT_STREAMHANDLE_CONTEXT failed. Status = %X\n", Status);
			return FALSE;
		}

		RtlZeroMemory(Context, sizeof(EPT_STREAMHANDLE_CONTEXT));

		break;
	}

	}
	

	*CompletionContext = Context;

	return TRUE;

}


BOOLEAN EptGetOrSetContext(IN PFLT_INSTANCE Instance, IN PFILE_OBJECT FileObject, IN OUT PEPT_STREAM_CONTEXT* CompletionContext, IN FLT_CONTEXT_TYPE ContextType) {

	NTSTATUS Status = 0;
	PFLT_CONTEXT NewContext = NULL, OldContext = NULL;

	ASSERT(CompletionContext != NULL);
	
	NewContext = *CompletionContext;

	
	switch (ContextType) {

	case FLT_STREAM_CONTEXT:
	{
		Status = FltGetStreamContext(Instance, FileObject, &OldContext);
		break;
	}
	case FLT_STREAMHANDLE_CONTEXT:
	{
		Status = FltGetStreamHandleContext(Instance, FileObject, &OldContext);
		break;
	}

	}



	if (Status == STATUS_NOT_FOUND) {

		//����FltSetStream/HandleContext
	}
	else if (!NT_SUCCESS(Status)) {

		//DbgPrint("[EptGetOrSetContext]->FltGetStream/StreamHandle Context failed. Status = %x\n", Status);
		return FALSE;
	}
	else {

		//�ҵ��Ѿ����õ�context

		if (NewContext != NULL) {

			FltReleaseContext(NewContext);
			NewContext = NULL;
		}

		*CompletionContext = OldContext;


		return TRUE;
	}
	
	
	switch (ContextType) {

	case FLT_STREAM_CONTEXT:
	{
		Status = FltSetStreamContext(Instance, FileObject, FLT_SET_CONTEXT_KEEP_IF_EXISTS, NewContext, &OldContext);
		break;
	}
	case FLT_STREAMHANDLE_CONTEXT:
	{
		Status = FltSetStreamHandleContext(Instance, FileObject, FLT_SET_CONTEXT_KEEP_IF_EXISTS, NewContext, &OldContext);
		break;
	}

	}



	if (!NT_SUCCESS(Status)) {

		FltReleaseContext(NewContext);

		if (Status == STATUS_FLT_CONTEXT_ALREADY_DEFINED) {

			//  We're racing with some other call which managed to set the
			//  context before us. We will return that context instead, which
			//  will be in oldContext.

			*CompletionContext = OldContext;


			return TRUE;
		}

		DbgPrint("[EptGetOrSetContext]->FltGetStream/StreamHandle Context failed twice. Status = %x\n", Status);
		*CompletionContext = NULL;
		return FALSE;
	}

	*CompletionContext = NewContext;

	return TRUE;

}