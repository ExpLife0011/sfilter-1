#include "precomp.h"

extern ULONG g_ulWaitID;
extern LIST_ENTRY g_OperList;	//�����б����ٸ��ֱ���ص���Ϊ
extern ERESOURCE  g_OperListLock;

extern LIST_ENTRY g_WaitList;	//�ȴ��������������ֹ���б�
extern ERESOURCE  g_WaitListLock;

extern LIST_ENTRY g_PendingIrpList; //�����������б�����ݱ������IRP
extern ERESOURCE  g_PendingIrpListLock;


/////////////////////////////////////////////////////////////////////////////
//
//                  HIPS Communication Routines
//
/////////////////////////////////////////////////////////////////////////////

WAIT_LIST_ENTRY*
FindWaitEntryByID(PLIST_ENTRY pListHead, ULONG ulWaitID)
{
    PLIST_ENTRY			pList		= NULL;
    WAIT_LIST_ENTRY		*pEntry	= NULL;
	
    for (pList = pListHead->Flink; pList != pListHead; pList = pList->Flink)
    {
        pEntry = CONTAINING_RECORD(pList, WAIT_LIST_ENTRY, m_List);
        if (pEntry->m_ulWaitID == ulWaitID)
        {
            return pEntry;
        }
    }
    return NULL;
}

ULONG MakeWaitID()
{
    InterlockedIncrement(&g_ulWaitID);
    return g_ulWaitID;
}

BOOLEAN
CompletePendingIrp(LIST_ENTRY* pIrpListHead, 
						OP_INFO* lpOpInfo)
{
	LIST_ENTRY		*pIrpList	= NULL;
	PIRP			pIrp		= NULL;
	BOOLEAN			bFound		= FALSE;
	BOOLEAN			bRet		= FALSE;
	
	if (IsListEmpty(pIrpListHead) == TRUE)
	{
		return bRet;
	}
	
	for (pIrpList = pIrpListHead->Flink; pIrpList != pIrpListHead; pIrpList = pIrpList->Flink)
	{
		pIrp = CONTAINING_RECORD(pIrpList, IRP, Tail.Overlay.ListEntry);
		if (IoSetCancelRoutine(pIrp, NULL))
		{
			RemoveEntryList(pIrpList);
			bFound = TRUE;
			break;
		}
	}
	
	if (bFound == FALSE)
	{
		return bRet;
	}
	
	RtlCopyMemory(pIrp->AssociatedIrp.SystemBuffer, lpOpInfo, sizeof(RING3_OP_INFO));
	
	pIrp->IoStatus.Information = sizeof(RING3_OP_INFO);
	pIrp->IoStatus.Status = STATUS_SUCCESS;
	
	IoCompleteRequest(pIrp, IO_NO_INCREMENT);
	bRet = TRUE;
	
	return bRet;
}


USER_RESULT 
__stdcall
hipsGetResultFromUser(WCHAR *szOperType, 
					WCHAR *szTarget, 
					WCHAR *szTargetEx, 
					USER_RESULT DefaultAction)
{
    USER_RESULT			NotifyResult		= User_Pass;
    BOOLEAN				bSuccess			= FALSE;
    NTSTATUS			Status				= STATUS_SUCCESS;
    LARGE_INTEGER		WaitTimeOut			= {0};
    OP_INFO				*lpNewOpInfo		= NULL;
    WAIT_LIST_ENTRY		*lpNewWaitEntry		= NULL;
    ULONG_PTR			ulPtr				= 0;

	UNICODE_STRING		ustrProcessName		= {0};
 
    lpNewOpInfo = (OP_INFO*)ExAllocatePoolWithTag(PagedPool, sizeof(OP_INFO), 'NIPO');

    if (lpNewOpInfo == NULL)
    {
        return NotifyResult;
    }

	RtlZeroMemory(lpNewOpInfo, sizeof(OP_INFO));

	ustrProcessName.Buffer = lpNewOpInfo->m_ProcessName;
	ustrProcessName.Length = 0;
	ustrProcessName.MaximumLength = MAX_PATH*sizeof(WCHAR);

	Status = GetProcessFullNameByPid(PsGetCurrentProcessId(), &ustrProcessName);

	if (!NT_SUCCESS(Status) /*|| wcsstr(lpNewOpInfo->m_ProcessName, L"explorer.exe") == NULL*/) //ֻ���Explorer.exe
	{
		return NotifyResult;
	}

	DbgPrint("proc:%wZ\n", &ustrProcessName);

	if (szOperType != NULL)
	{
		RtlCopyMemory(lpNewOpInfo->m_szOper, szOperType, sizeof(lpNewOpInfo->m_szOper));
	}

	if (szTarget != NULL)
	{
		wcsncpy(lpNewOpInfo->m_TargetName,szTarget, MAX_PATH - 1);
	}
    lpNewOpInfo->m_TargetName[MAX_PATH - 1] = UNICODE_NULL;
    lpNewOpInfo->m_TargetNameEx[0] = UNICODE_NULL;
    if (szTargetEx != NULL)
    {
        wcsncpy(lpNewOpInfo->m_TargetNameEx, szTargetEx, MAX_PATH - 1);
        lpNewOpInfo->m_TargetNameEx[MAX_PATH - 1] = UNICODE_NULL;
    }

    lpNewOpInfo->m_ulWaitID = MakeWaitID();
  

    lpNewWaitEntry = (WAIT_LIST_ENTRY*)ExAllocatePoolWithTag(NonPagedPool, 
		sizeof(WAIT_LIST_ENTRY), 'TIAW');
    if (lpNewWaitEntry == NULL)
    {
        goto _EXIT;
    }

    lpNewWaitEntry->m_ulWaitID = lpNewOpInfo->m_ulWaitID;
    KeInitializeEvent(&lpNewWaitEntry->m_ulWaitEvent, SynchronizationEvent, FALSE);
	
    // ����ȴ�����
    LockWrite(&g_WaitListLock);
	InsertTailList(&g_WaitList, &lpNewWaitEntry->m_List);
    UnLockWrite(&g_WaitListLock);

	// ��40�룬��3��30�볬ʱ
    WaitTimeOut.QuadPart = -40 * 10000000;

    LockWrite(&g_PendingIrpListLock);
    bSuccess = CompletePendingIrp(&g_PendingIrpList, lpNewOpInfo);
    UnLockWrite(&g_PendingIrpListLock);

	if (bSuccess == FALSE)	//���pending irpʧ�ܣ���lpNewFileOpInfo����operlist
	{
        // ������ɹ�, ��ô�Ŷӵ�ͬ����Ϣ����, �������ȴ���
        LockWrite(&g_OperListLock);
        InsertTailList(&g_OperList, &lpNewOpInfo->m_List);
        UnLockWrite(&g_OperListLock);
        // һ�������, ��߾Ͳ������ͷ���
        lpNewOpInfo = NULL;
	}

	Status = KeWaitForSingleObject(&lpNewWaitEntry->m_ulWaitEvent, 
		Executive, KernelMode, FALSE, &WaitTimeOut);

    LockWrite(&g_WaitListLock);
    RemoveEntryList(&lpNewWaitEntry->m_List);
    UnLockWrite(&g_WaitListLock);

    if (Status != STATUS_TIMEOUT)
    {
        if (lpNewWaitEntry->m_bBlocked == TRUE)
        {
            NotifyResult = User_Block;
        }
        else
        {
            NotifyResult = User_Pass;
        }
    }
    else
    {
        NotifyResult = DefaultAction;
    }

_EXIT:
    if (lpNewWaitEntry != NULL)
    {
        ExFreePoolWithTag(lpNewWaitEntry, 'TIAW');
    }
    if (lpNewOpInfo != NULL)
    {
        ExFreePoolWithTag(lpNewOpInfo, 'NIPO');
    }
    return NotifyResult;
}

