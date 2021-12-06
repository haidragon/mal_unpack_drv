#pragma once

#include "main.h"
#include "data_structs.h"


namespace Data {
    bool AllocGlobals();

    void FreeGlobals();

    bool ContainsFile(LONGLONG fileId);

    ULONG GetFileOwner(LONGLONG fileId);

    ULONG  GetProcessOwner(ULONG pid);

    bool ContainsProcess(ULONG pid);

    bool AreSameFamily(ULONG pid1, ULONG pid2);

    t_add_status AddFile(LONGLONG fileId, ULONG parentPid);

    t_add_status AddProcess(ULONG pid, ULONG parentPid);

    t_add_status AddProcessToFileOwner(ULONG PID, LONGLONG fileId);

    bool IsProcessInFileOwners(ULONG pid1, LONGLONG fileId);

    ULONGLONG CountProcesses();

    bool DeleteProcess(ULONG pid);

    size_t CopyProcessList(ULONG rootPid, void* data, size_t outBufSize);

    size_t CopyFilesList(ULONG rootPid, void* data, size_t outBufSize);

    NTSTATUS WaitForProcessDeletion(ULONG pid, PLARGE_INTEGER checkInterval);
};
