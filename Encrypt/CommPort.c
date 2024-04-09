
#pragma warning(disable:4996)
#include "commport.h"
#include "processverify.h"
#include "linkedList.h"
#include "privilegeendecrypt.h"

PFLT_PORT gServerPort;
PFLT_PORT gClientPort;
WCHAR g_wFileName[260] = { 0 };


NTSTATUS ConnectNotifyCallback(IN PFLT_PORT ClientPort, IN PVOID ServerPortCookie, IN PVOID ConnectionContext, IN ULONG SizeOfContext, IN PVOID* ConnectionPortCookie)
{

	UNREFERENCED_PARAMETER(ServerPortCookie);
	UNREFERENCED_PARAMETER(ConnectionContext);
	UNREFERENCED_PARAMETER(SizeOfContext);
	UNREFERENCED_PARAMETER(ConnectionPortCookie);

	PAGED_CODE();

	DbgPrint("ConnectNotifyCallback->connect with user.\n");

	gClientPort = ClientPort;

	return STATUS_SUCCESS;
}


VOID DisconnectNotifyCallback(IN PVOID ConnectionCookie)
{
	UNREFERENCED_PARAMETER(ConnectionCookie);

	PAGED_CODE();

	DbgPrint("DisconnectNotifyCallback->disconnect with user.\n");

	FltCloseClientPort(gFilterHandle, &gClientPort);
}


NTSTATUS MessageNotifyCallback(IN PVOID PortCookie, IN PVOID InputBuffer, IN ULONG InputBufferLength, IN PVOID OutputBuffer, IN ULONG OutputBufferLength, OUT PULONG ReturnOutputBufferLength)
{
	UNREFERENCED_PARAMETER(PortCookie);
	UNREFERENCED_PARAMETER(InputBufferLength);
	UNREFERENCED_PARAMETER(OutputBuffer);
	UNREFERENCED_PARAMETER(OutputBufferLength);
	UNREFERENCED_PARAMETER(ReturnOutputBufferLength);

	PAGED_CODE();

	PUCHAR Buffer;
	EPT_MESSAGE_HEADER MessageHeader;
	NTSTATUS Status = STATUS_UNSUCCESSFUL;

	if (InputBuffer != NULL)
	{

		try
		{
			Buffer = InputBuffer;

		}
		except(EXCEPTION_EXECUTE_HANDLER)
		{
			return GetExceptionCode();
		}

		RtlMoveMemory(&MessageHeader, Buffer, sizeof(EPT_MESSAGE_HEADER));

		switch (MessageHeader.Command)
		{
		case EPT_HELLO_KERNEL:
		{
			DbgPrint("%s", (Buffer + sizeof(EPT_MESSAGE_HEADER)));
			break;
		}
		case EPT_INSERT_PROCESS_RULES:
		{
			//DbgPrint("%s", (Buffer + sizeof(EPT_MESSAGE_HEADER)));

			PEPT_PROCESS_RULES ProcessRules;
			ProcessRules = ExAllocatePoolWithTag(PagedPool, sizeof(EPT_PROCESS_RULES), PROCESS_RULES_BUFFER_TAG);
			if (!ProcessRules)
			{
				DbgPrint("MessageNotifyCallback ExAllocatePoolWithTag ProcessRules failed.\n");
				return 0;
			}

			RtlZeroMemory(ProcessRules, sizeof(EPT_PROCESS_RULES));

			RtlMoveMemory(ProcessRules, Buffer + sizeof(EPT_MESSAGE_HEADER), sizeof(EPT_PROCESS_RULES));

			EPT_PROCESS_RULES TempPR = { 0 };
			RtlMoveMemory(TempPR.TargetProcessName, ProcessRules->TargetProcessName, strlen(ProcessRules->TargetProcessName));

			Status = EptIsPRInLinkedList(&TempPR);

			//������ָ�����㣬Ϊ֮��ıȽ�
			RtlZeroMemory(&TempPR, sizeof(LIST_ENTRY));

			if (EPT_STATUS_TARGET_MATCH == Status)
			{
				if (!strncmp(ProcessRules->TargetExtension, TempPR.TargetExtension, sizeof(ProcessRules->TargetExtension)) &&
					!strncmp((PCHAR)ProcessRules->Hash, (PCHAR)TempPR.Hash, sizeof(ProcessRules->Hash)) &&
					ProcessRules->IsCheckHash == TempPR.IsCheckHash &&
					ProcessRules->Access == TempPR.Access)
				{
					DbgPrint("MessageNotifyCallback->%s PR already exsits.\n", ProcessRules->TargetProcessName);

					if (NULL != ProcessRules)
					{
						ExFreePoolWithTag(ProcessRules, PROCESS_RULES_BUFFER_TAG);
						ProcessRules = NULL;
					}

					Status = EPT_SAME_PR_ALREADY_EXISTS;

					EPT_MESSAGE_HEADER SendBuffer = { 0 };
					SendBuffer.Command = Status;
					Status = FltSendMessage(gFilterHandle, &gClientPort, &SendBuffer, sizeof(EPT_MESSAGE_HEADER), NULL, NULL, NULL);
					if (STATUS_SUCCESS != Status)
					{
						DbgPrint("MessageNotifyCallback->FltSendMessage failed status = 0x%x.\n", Status);
					}
					else
					{
						DbgPrint("MessageNotifyCallback->FltSendMessage success.\n");
					}
					
					break;
				}
				else
				{
					Status = EptReplacePRInLinkedList(*ProcessRules);

					DbgPrint("MessageNotifyCallback->EptReplacePRInLinkedList %s.\n", ProcessRules->TargetProcessName);

					if (STATUS_SUCCESS != Status)
					{
						DbgPrint("MessageNotifyCallback->EptReplacePRInLinkedList %s failed.\n", ProcessRules->TargetProcessName);
						if (NULL != ProcessRules)
						{
							ExFreePoolWithTag(ProcessRules, PROCESS_RULES_BUFFER_TAG);
							ProcessRules = NULL;
						}
						break;
					}

					if (NULL != ProcessRules)
					{
						ExFreePoolWithTag(ProcessRules, PROCESS_RULES_BUFFER_TAG);
						ProcessRules = NULL;
					}

					Status = EPT_UPDATE_PR;

					EPT_MESSAGE_HEADER SendBuffer = { 0 };
					SendBuffer.Command = Status;
					Status = FltSendMessage(gFilterHandle, &gClientPort, &SendBuffer, sizeof(EPT_MESSAGE_HEADER), NULL, NULL, NULL);
					if (STATUS_SUCCESS != Status)
					{
						DbgPrint("MessageNotifyCallback->FltSendMessage failed status = 0x%x.\n", Status);
					}
					else
					{
						DbgPrint("MessageNotifyCallback->FltSendMessage success.\n");
					}

					break;
				}

			}

			DbgPrint("MessageNotifyCallback->InsertTailList ProcessRules->TargetProcessName = %s.\n", ProcessRules->TargetProcessName);
			ExInterlockedInsertTailList(&ProcessRulesListHead, &ProcessRules->ListEntry, &ProcessRulesListSpinLock);

			Status = EPT_INSERT_PR;

			EPT_MESSAGE_HEADER SendBuffer = { 0 };
			SendBuffer.Command = Status;
			Status = FltSendMessage(gFilterHandle, &gClientPort, &SendBuffer, sizeof(EPT_MESSAGE_HEADER), NULL, NULL, NULL);
			if (STATUS_SUCCESS != Status)
			{
				DbgPrint("MessageNotifyCallback->FltSendMessage failed status = 0x%x.\n", Status);
			}
			else
			{
				DbgPrint("MessageNotifyCallback->FltSendMessage success.\n");
			}

			break;
		}
		case EPT_PRIVILEGE_DECRYPT:
		{
			EPT_MESSAGE_PRIV_DECRYPT PrivDecrypt = { 0 };
			ANSI_STRING Ansi = { 0 };
			UNICODE_STRING uFileName = { 0 };

			uFileName.MaximumLength = sizeof(g_wFileName);
			RtlZeroMemory(g_wFileName, sizeof(g_wFileName));
			uFileName.Buffer = g_wFileName;

			RtlMoveMemory(PrivDecrypt.FileName, Buffer + sizeof(EPT_MESSAGE_HEADER), strlen((PCHAR)Buffer + sizeof(EPT_MESSAGE_HEADER)));

			DbgPrint("MessageNotifyCallback->EPT_PRIVILEGE_DECRYPT FileName = %s.\n", PrivDecrypt.FileName);

			RtlInitAnsiString(&Ansi, PrivDecrypt.FileName);

			Status = RtlAnsiStringToUnicodeString(&uFileName, &Ansi, FALSE);

			if (STATUS_SUCCESS != Status)
			{
				DbgPrint("MessageNotifyCallback->EPT_PRIVILEGE_DECRYPT->RtlAnsiStringToUnicodeString failed status = 0x%x.\n", Status);
				/*return Status;*/
			}

			Status = EptPrivilegeEnDecrypt(&uFileName, EPT_PRIVILEGE_DECRYPT);

			if (STATUS_SUCCESS != Status)
			{
				DbgPrint("MessageNotifyCallback->EPT_PRIVILEGE_DECRYPT->EptPrivilegeEnDecrypt failed status = 0x%x.\n", Status);
				break;
			}

			break;
		}
		case EPT_PRIVILEGE_ENCRYPT:
		{
			EPT_MESSAGE_PRIV_DECRYPT PrivDecrypt = { 0 };
			ANSI_STRING Ansi = { 0 };
			UNICODE_STRING uFileName = { 0 };

			uFileName.MaximumLength = sizeof(g_wFileName);
			RtlZeroMemory(g_wFileName, sizeof(g_wFileName));
			uFileName.Buffer = g_wFileName;

			RtlMoveMemory(PrivDecrypt.FileName, Buffer + sizeof(EPT_MESSAGE_HEADER), strlen((PCHAR)Buffer + sizeof(EPT_MESSAGE_HEADER)));

			DbgPrint("MessageNotifyCallback->EPT_PRIVILEGE_ENCRYPT FileName = %s.\n", PrivDecrypt.FileName);

			RtlInitAnsiString(&Ansi, PrivDecrypt.FileName);

			Status = RtlAnsiStringToUnicodeString(&uFileName, &Ansi, FALSE);

			if (STATUS_SUCCESS != Status)
			{
				DbgPrint("MessageNotifyCallback->EPT_PRIVILEGE_ENCRYPT->RtlAnsiStringToUnicodeString failed status = 0x%x.\n", Status);
				/*return Status;*/
			}

			Status = EptPrivilegeEnDecrypt(&uFileName, EPT_PRIVILEGE_ENCRYPT);

			if (STATUS_SUCCESS != Status)
			{
				DbgPrint("MessageNotifyCallback->EPT_PRIVILEGE_ENCRYPT->EptPrivilegeEnDecrypt failed status = 0x%x.\n", Status);
				break;
			}

			break;
		}
		}

	}

	
	
	return STATUS_SUCCESS;
}


BOOLEAN EptInitCommPort()
{

	NTSTATUS Status;
	PSECURITY_DESCRIPTOR SecurityDescriptor;
	UNICODE_STRING CommPortName;
	OBJECT_ATTRIBUTES ObjectAttributes;

	Status = FltBuildDefaultSecurityDescriptor(&SecurityDescriptor, FLT_PORT_ALL_ACCESS);

	if(!NT_SUCCESS(Status))
	{
		DbgPrint("[EptInitCommPort]->FltBuildDefaultSecurityDescriptor failed. Status = %x\n", Status);
		return FALSE;
	}

	RtlInitUnicodeString(&CommPortName, L"\\Encrypt-hkx3upper");

	InitializeObjectAttributes(&ObjectAttributes, &CommPortName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, SecurityDescriptor);

	Status = FltCreateCommunicationPort(gFilterHandle, &gServerPort, &ObjectAttributes, NULL, ConnectNotifyCallback, DisconnectNotifyCallback, MessageNotifyCallback, 1);

	FltFreeSecurityDescriptor(SecurityDescriptor);

	if (!NT_SUCCESS(Status))
	{
		FltCloseCommunicationPort(gServerPort);
		DbgPrint("[EptInitCommPort]->FltCreateCommunicationPort failed. Status = %x\n", Status);
		return FALSE;
	}

	return TRUE;

}


VOID EptCloseCommPort()
{
	if (gServerPort)
	{
		FltCloseCommunicationPort(gServerPort);
	}
}