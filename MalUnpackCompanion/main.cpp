#include <ntifs.h>
#include <ntddk.h>

#include <fltKernel.h>
#include <dontuse.h>

#include "main.h"
#include "undoc_api.h"

#include "common.h"
#include "data_manager.h"
#include "filters.h"
#include "fs_filters.h"

#include "process_util.h"
#include "file_util.h"

#define SLEEP_TIME 1000

#define ALTITUDE_PROCESS_FILTER L"12345.6171"
#define ALTITUDE_REGISTRY_FILTER L"7657.124"

#define SUPPORTED_CLIENT_NAME L"\\mal_unpack.exe"
#define ONLY_SUPPORTED_CLIENT

#define IO_METHOD_FROM_CTL_CODE(cltCode) (cltCode & 0x00000003)

active_settings g_Settings;
//---


void _OnProcessCreation(_Inout_ PEPROCESS Process, _In_ HANDLE ProcessId, _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo)
{
	UNREFERENCED_PARAMETER(Process);
	const ULONG creatorPID = HandleToULong(PsGetCurrentProcessId()); //the PID creating the thread
	USHORT commandLineSize = 0;
	if (CreateInfo->CommandLine) {
		commandLineSize = CreateInfo->CommandLine->Length;
	}

	ULONG PID = HandleToULong(ProcessId);
	ULONG ParentPID = creatorPID;//HandleToULong(CreateInfo->ParentProcessId);
	if (Data::ContainsProcess(ParentPID)) {
		DbgPrint(DRIVER_PREFIX "[%d] created WATCHED process: [%d]\n", ParentPID, PID);
		if (commandLineSize) {
			DbgPrint(DRIVER_PREFIX "[%d] -> %S\n", PID, CreateInfo->CommandLine->Buffer);
		}
		if (Data::AddProcess(PID, ParentPID) == ADD_LIMIT_EXHAUSTED) {
			DbgPrint(DRIVER_PREFIX "[%d] Could not add to the watchlist: limit exhausted\n", PID);
			CreateInfo->CreationStatus = STATUS_ACCESS_DENIED;
		}
	}
}

void _OnProcessExit(_Inout_ PEPROCESS Process, _In_ HANDLE ProcessId)
{
	UNREFERENCED_PARAMETER(Process);

	const ULONG PID = HandleToULong(ProcessId);
	Data::WaitForProcessDeletion(PID, 0);
}

void OnProcessNotify(_Inout_ PEPROCESS Process, _In_ HANDLE ProcessId, _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo)
{
	if (CreateInfo) {
		//process created:
		_OnProcessCreation(Process, ProcessId, CreateInfo);
	}
	else {
		_OnProcessExit(Process, ProcessId);
	}
}

void OnThreadNotify(HANDLE ProcessId, HANDLE Thread, BOOLEAN Create)
{
	const ULONG creatorPID = HandleToULong(PsGetCurrentProcessId()); //the PID creating the thread
	const ULONG targetPID = HandleToULong(ProcessId);
	const ULONG ThreadId = HandleToULong(Thread);

	if (!Data::ContainsProcess(creatorPID)) {
		return;
	}
	if (Create && (creatorPID != targetPID)) {
		DbgPrint(DRIVER_PREFIX "[%d] THREAD: Creating remote thread! %d -> %d [%x]\n", creatorPID, creatorPID, targetPID, targetPID);
		if (Data::AddProcess(targetPID, creatorPID) == ADD_LIMIT_EXHAUSTED) {
			DbgPrint(DRIVER_PREFIX "[%d] Could not add to the watchlist: limit exhausted\n", targetPID);
		}
	}
}


void OnImageLoadNotify(PUNICODE_STRING FullImageName, HANDLE ProcessId, PIMAGE_INFO ImageInfo)
{
	UNREFERENCED_PARAMETER(ImageInfo);
	if (ProcessId == nullptr) {
		// system image, ignore
		return;
	}
	if (!FullImageName) { //Image name is empty
		return;
	}
	const ULONG PID = HandleToULong(ProcessId);
	LONGLONG fileId = FileUtil::GetFileIdByPath(FullImageName);
	if (fileId == FILE_INVALID_FILE_ID) {
		return;
	}

	const t_add_status aStat = Data::AddProcessToFileOwner(PID, fileId);
	if (aStat == ADD_INVALID_ITEM) {
		return; // this file is not owned by any process
	}
	if (aStat == ADD_LIMIT_EXHAUSTED) {
		DbgPrint(DRIVER_PREFIX "[%d] Could not add to the watchlist: limit exhausted\n");
	}
	if (aStat == ADD_OK) {
		DbgPrint(DRIVER_PREFIX __FUNCTION__" [%d] Added process created from the OWNED file-> %zX\n", PID, fileId);
	}
}

void UnregisterCallbacks()
{
	if (g_Settings.RegHandle) {
		ObUnRegisterCallbacks(g_Settings.RegHandle);
		g_Settings.RegHandle = NULL;
	}
	if (g_Settings.RegCookie.QuadPart != 0) {
		CmUnRegisterCallback(g_Settings.RegCookie);
		g_Settings.RegCookie.QuadPart = 0;
	}
	if (g_Settings.gFilterHandle) {
		FltUnregisterFilter(g_Settings.gFilterHandle);
		g_Settings.gFilterHandle = NULL;
	}
}

void MyDriverUnload(_In_ PDRIVER_OBJECT DriverObject)
{
	Data::FreeGlobals();

	//unregister the notification
	if (g_Settings.hasImageNotify) {
		PsRemoveLoadImageNotifyRoutine(OnImageLoadNotify);
		g_Settings.hasImageNotify = false;
	}
	if (g_Settings.hasThreadNotify) {
		PsRemoveCreateThreadNotifyRoutine(OnThreadNotify);
		g_Settings.hasThreadNotify = false;
	}
	if (g_Settings.hasProcessNotify) {
		PsSetCreateProcessNotifyRoutineEx(OnProcessNotify, TRUE);
		g_Settings.hasProcessNotify = false;
	}

	UnregisterCallbacks();

	UNICODE_STRING symLink = RTL_CONSTANT_STRING(MY_DRIVER_LINK);
	// delete symbolic link
	IoDeleteSymbolicLink(&symLink);

	// delete device object
	IoDeleteDevice(DriverObject->DeviceObject);

	DbgPrint(DRIVER_PREFIX "driver unloaded!\n");
}


NTSTATUS HandleCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
	UNREFERENCED_PARAMETER(DeviceObject);

#ifdef  ONLY_SUPPORTED_CLIENT
	NTSTATUS openStatus = STATUS_ACCESS_DENIED;
	const ULONG sourcePID = HandleToULong(PsGetCurrentProcessId()); //the PID of the process performing the operation
	
	PEPROCESS Process;
	NTSTATUS status = PsLookupProcessByProcessId(ULongToHandle(sourcePID), &Process);
	if (NT_SUCCESS(status)) {
		//TODO: make more fancy check:
		if (ProcessUtil::CheckProcessPath(Process, SUPPORTED_CLIENT_NAME)) {
			openStatus = STATUS_SUCCESS;
		}
		ObDereferenceObject(Process);
	}

	if (openStatus != STATUS_SUCCESS) {
		DbgPrint(DRIVER_PREFIX "[%d] ACCESS DENIED: cannot open the driver with this process\n", sourcePID);
	}
#else
	NTSTATUS openStatus = STATUS_SUCCESS;
#endif //  ONLY_SUPPORTED_CLIENT

	Irp->IoStatus.Status = openStatus;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return openStatus;
}

NTSTATUS FetchInputBuffer(PIRP Irp, ProcessData** inpData)
{
	if (!Irp || inpData == nullptr) {
		return STATUS_UNSUCCESSFUL;
	}
	PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
	ULONG method = IO_METHOD_FROM_CTL_CODE(stack->Parameters.DeviceIoControl.IoControlCode);

	if (method == METHOD_BUFFERED) {
		if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(ProcessData)) {
			return STATUS_BUFFER_TOO_SMALL;
		}
		*inpData = (ProcessData*)Irp->AssociatedIrp.SystemBuffer;
	}
	else if (method == METHOD_NEITHER) {
		if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(ProcessData)) {
			return STATUS_BUFFER_TOO_SMALL;
		}
		*inpData = (ProcessData*)stack->Parameters.DeviceIoControl.Type3InputBuffer;
	}
	else {
		return STATUS_NOT_SUPPORTED;
	}
	if (*inpData == nullptr) {
		return STATUS_INVALID_PARAMETER;
	}
	return STATUS_SUCCESS; 
}

NTSTATUS _add_process_watch(ULONG PID)
{
	PEPROCESS Process;
	NTSTATUS status = PsLookupProcessByProcessId(ULongToHandle(PID), &Process);
	if (!NT_SUCCESS(status)) {
		DbgPrint(DRIVER_PREFIX "Such process does not exist: %d\n", PID);
		return STATUS_INVALID_PARAMETER;
	}
	ObDereferenceObject(Process);

	DbgPrint(DRIVER_PREFIX "Watching process requested %d\n", PID);
	t_add_status ret = Data::AddProcess(PID, 0);
	if (ret == ADD_OK || ret == ADD_ALREADY_EXIST) {
		ULONGLONG count = Data::CountProcesses();
		DbgPrint(DRIVER_PREFIX "Added to the list, list size=%zd\n", count);
	}
	else {
		ULONGLONG count = Data::CountProcesses();
		DbgPrint(DRIVER_PREFIX "failed to add process to the list, size=%zd, ret = %d\n", count, ret);
	}
	if (Data::ContainsProcess(PID)) {
		DbgPrint(DRIVER_PREFIX "The PID exists on the list %d\n", PID);
	}
	return STATUS_SUCCESS;
}

NTSTATUS _terminate_watched(ULONG PID)
{
	if (!Data::ContainsProcess(PID)) {
		return 0;
	}
	NTSTATUS status = ProcessUtil::TerminateProcess(PID);
	Data::DeleteProcess(PID);
	return status;
}

//---
NTSTATUS AddProcessWatch(PIRP Irp)
{
	ProcessData* inpData = nullptr;
	NTSTATUS status = FetchInputBuffer(Irp, &inpData);
	if (!NT_SUCCESS(status)) {
		return status;
	}
	ULONG pid = inpData->Id;
	return _add_process_watch(pid);
}

NTSTATUS RemoveProcessWatch(PIRP Irp)
{
	ProcessData* inpData = nullptr;

	NTSTATUS status = FetchInputBuffer(Irp, &inpData);
	if (!NT_SUCCESS(status)) {
		return status;
	}
	const ULONG PID = inpData->Id;
	DbgPrint(DRIVER_PREFIX "Removing process watch: %d\n", PID);
	if (Data::DeleteProcess(PID)) {
		DbgPrint(DRIVER_PREFIX "Removed from the list: %d\n", PID);
	}
	return STATUS_SUCCESS;
}

NTSTATUS TerminateWatched(PIRP Irp)
{
	ProcessData* inpData = nullptr;

	NTSTATUS status = FetchInputBuffer(Irp, &inpData);
	if (!NT_SUCCESS(status)) {
		return status;
	}
	return _terminate_watched(inpData->Id);
}

NTSTATUS _CopyWatchedList(PIRP Irp, ULONG_PTR& outLen, bool files)
{
	ProcessData* inpData = nullptr;
	NTSTATUS status = FetchInputBuffer(Irp, &inpData);
	if (!NT_SUCCESS(status)) {
		return status;
	}
	
	const size_t elementSize = files ? sizeof(LONGLONG) : sizeof(ULONG);

	PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
	const size_t outBufSize = stack->Parameters.DeviceIoControl.OutputBufferLength;
	if (outBufSize < elementSize) {
		return STATUS_BUFFER_TOO_SMALL;
	}
	void* outData = Irp->AssociatedIrp.SystemBuffer;
	if (outData == nullptr) {
		return STATUS_INVALID_PARAMETER;
	}
	ULONG parentPid = inpData->Id;
	size_t items = 0;
	if (files) {
		items = Data::CopyFilesList(parentPid, outData, outBufSize);
	}
	else {
		items = Data::CopyProcessList(parentPid, outData, outBufSize);
	}
	if (items > 0) {
		DbgPrint(DRIVER_PREFIX "Copied items to system buffer: %d\n", items);
		size_t copiedSize = items * elementSize;
		outLen = ULONG(copiedSize);
	}
	return STATUS_SUCCESS;
}

NTSTATUS CopyProcessesList(PIRP Irp, ULONG_PTR& outLen)
{
	return _CopyWatchedList(Irp, outLen, false);
}

NTSTATUS CopyFilesList(PIRP Irp, ULONG_PTR& outLen)
{
	return _CopyWatchedList(Irp, outLen, true);
}

NTSTATUS HandleDeviceControl(PDEVICE_OBJECT, PIRP Irp)
{
	PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
	
	NTSTATUS status = STATUS_SUCCESS;

	ULONG_PTR outLen = 0;
	switch (stack->Parameters.DeviceIoControl.IoControlCode) {
		case IOCTL_MUNPACK_COMPANION_ADD_TO_WATCHED:
		{
			status = AddProcessWatch(Irp);
			break;
		}
#ifdef ALLOW_DELETE
		case IOCTL_MUNPACK_COMPANION_REMOVE_FROM_WATCHED:
		{
			status = RemoveProcessWatch(Irp);
			break;
		}
#endif
		case IOCTL_MUNPACK_COMPANION_TERMINATE_WATCHED:
		{
			status = TerminateWatched(Irp);
			break;
		}
		case IOCTL_MUNPACK_COMPANION_LIST_PROCESSES:
		{
			status = CopyProcessesList(Irp, outLen);
			break;
		}
		case IOCTL_MUNPACK_COMPANION_LIST_FILES:
		{
			status = CopyFilesList(Irp, outLen);
			break;
		}
		default:
			status = STATUS_INVALID_DEVICE_REQUEST;
			break;
	}

	Irp->IoStatus.Status = status;
	Irp->IoStatus.Information = outLen;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return status;
}


NTSTATUS _RegisterOpenProcessCallbacks()
{
	OB_OPERATION_REGISTRATION operations[] = {
		{
			PsProcessType,		// object type
			OB_OPERATION_HANDLE_CREATE | OB_OPERATION_HANDLE_DUPLICATE,
			OnPreOpenProcess, nullptr	// pre, post
		}
	};
	OB_CALLBACK_REGISTRATION reg = {
		OB_FLT_REGISTRATION_VERSION,
		1,				// operation count
		RTL_CONSTANT_STRING(ALTITUDE_PROCESS_FILTER),	// altitude
		nullptr,		// context
		operations
	};

	NTSTATUS status = ObRegisterCallbacks(&reg, &g_Settings.RegHandle);
	if (!NT_SUCCESS(status)) {
		DbgPrint(DRIVER_PREFIX "failed to register callbacks (status=%08X)\n", status);
		g_Settings.RegHandle = NULL;
		return status;
	}
	return status;
}


NTSTATUS _RegisterRegistryCallbacks(_In_ PDRIVER_OBJECT DriverObject)
{
	UNICODE_STRING altitude = RTL_CONSTANT_STRING(ALTITUDE_REGISTRY_FILTER);
	NTSTATUS status = CmRegisterCallbackEx(OnRegistryNotify, &altitude, DriverObject, nullptr, &g_Settings.RegCookie, nullptr);
	if (!NT_SUCCESS(status)) {
		DbgPrint(DRIVER_PREFIX "failed to set registry callback (%08X)\n", status);
		g_Settings.RegCookie.QuadPart = 0;
		return status;
	}
	return status;
}

NTSTATUS RegisterCallbacks(_In_ PDRIVER_OBJECT DriverObject)
{
	NTSTATUS status = _RegisterOpenProcessCallbacks();

	// Registry operations filtering:
	if (NT_SUCCESS(status)) {
		status = _RegisterRegistryCallbacks(DriverObject);
	}
	return status;
}

///

extern "C"
NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath) 
{
	UNREFERENCED_PARAMETER(RegistryPath);
	g_Settings.init();

	//
	//  Register with FltMgr to tell it our callback routines
	//
	NTSTATUS status = FltRegisterFilter(DriverObject,
		&FilterRegistration,
		&g_Settings.gFilterHandle);

	FLT_ASSERT(NT_SUCCESS(status));

	if (NT_SUCCESS(status)) {
		status = FltStartFiltering(g_Settings.gFilterHandle);
		if (!NT_SUCCESS(status)) {
			FltUnregisterFilter(g_Settings.gFilterHandle);
			g_Settings.gFilterHandle = NULL;
		}
	}

	status = RegisterCallbacks(DriverObject);
	if (!NT_SUCCESS(status)) {
		UnregisterCallbacks();
		return status;
	}

	DriverObject->DriverUnload = MyDriverUnload;
	DriverObject->MajorFunction[IRP_MJ_CREATE] = HandleCreateClose;
	DriverObject->MajorFunction[IRP_MJ_CLOSE] = HandleCreateClose;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = HandleDeviceControl;

	DbgPrint(DRIVER_PREFIX "driver loaded!\n");
	RTL_OSVERSIONINFOW version = { 0 };
	RtlGetVersion(&version);
	DbgPrint(DRIVER_PREFIX "OS Version: %d.%d.%d\n", version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber);
	DbgPrint(DRIVER_PREFIX "Driver Version: %s\n", VER_FILEVERSION_STR);

	UNICODE_STRING devName = RTL_CONSTANT_STRING(MY_DEVICE);

	PDEVICE_OBJECT DeviceObject = nullptr;
	status = IoCreateDevice(DriverObject, 0, &devName, FILE_DEVICE_UNKNOWN, 0, FALSE, &DeviceObject);
	if (!NT_SUCCESS(status)) {
		DbgPrint(DRIVER_PREFIX "Failed to create device (0x%08X)\n", status);
		return status;
	}

	UNICODE_STRING symLink = RTL_CONSTANT_STRING(MY_DRIVER_LINK);
	status = IoCreateSymbolicLink(&symLink, &devName);
	if (!NT_SUCCESS(status)) {
		DbgPrint(DRIVER_PREFIX "Failed to create symbolic link (0x%08X)\n", status);
		IoDeleteDevice(DeviceObject);
		return status;
	}

	status = PsSetCreateProcessNotifyRoutineEx(OnProcessNotify, FALSE);
	if (NT_SUCCESS(status)) {
		g_Settings.hasProcessNotify = true;
	} else {
		KdPrint((DRIVER_PREFIX "failed to register process callback (0x%08X)\n", status));
		IoDeleteSymbolicLink(&symLink);
		IoDeleteDevice(DeviceObject);
		return status;
	}

	status = PsSetCreateThreadNotifyRoutine(OnThreadNotify);
	if (NT_SUCCESS(status)) {
		g_Settings.hasThreadNotify = true;
	}
	else {
		DbgPrint(DRIVER_PREFIX "failed to set thread callback (status=%08X)\n", status);
	}

	status = PsSetLoadImageNotifyRoutine(OnImageLoadNotify);
	if (NT_SUCCESS(status)) {
		g_Settings.hasImageNotify = true;
	}
	else {
		DbgPrint(DRIVER_PREFIX "failed to set image notify callback (status=%08X)\n", status);
	}

	if (!Data::AllocGlobals()) {
		DbgPrint(DRIVER_PREFIX "Failed to initialize global data structures\n");
		MyDriverUnload(DriverObject);
		return STATUS_UNSUCCESSFUL;
	}
	else {
		DbgPrint(DRIVER_PREFIX "Initialized global data structures!\n");
	}

	DbgPrint(DRIVER_PREFIX "DriverEntry completed successfully\n");
	return STATUS_SUCCESS;
}
