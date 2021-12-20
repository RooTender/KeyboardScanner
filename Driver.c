#include <ntddk.h>

#define DEVICE_READ		CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_DATA)

UNICODE_STRING DeviceName = RTL_CONSTANT_STRING(L"\\Device\\KeyboardScanner");
UNICODE_STRING SymLinkName = RTL_CONSTANT_STRING(L"\\??\\keyboardscanner");
PDEVICE_OBJECT DeviceObject = NULL;

typedef struct {
	PDEVICE_OBJECT lowerKeyboardExtension;
} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

PDEVICE_OBJECT keyboardExtension = NULL;

typedef struct _KEYBOARD_INPUT_DATA {
	USHORT UnitId;
	USHORT MakeCode;
	USHORT Flags;
	USHORT Reserved;
	ULONG  ExtraInformation;
} KEYBOARD_INPUT_DATA, * PKEYBOARD_INPUT_DATA;

ULONG pendingIrp = 0;


VOID DriverUnload(PDRIVER_OBJECT DriverObject)
{
	// Remove device
	IoDeleteSymbolicLink(&SymLinkName);
	IoDeleteDevice(DeviceObject);

	// Detach keyboard
	PDEVICE_OBJECT DeviceObject = DriverObject->DeviceObject;
	IoDetachDevice(
		((PDEVICE_EXTENSION)DeviceObject->DeviceExtension)->lowerKeyboardExtension
	);
	IoDeleteDevice(keyboardExtension);

	LARGE_INTEGER interval = { 0 };
	interval.QuadPart = -10 * 1000 * 1000;

	while (pendingIrp) {
		KeDelayExecutionThread(KernelMode, FALSE, &interval);
	}

	DbgPrint("kbScanner: Driver unloaded\n");
}

NTSTATUS DispatchPass(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	if (DeviceObject->Type == FILE_DEVICE_KEYBOARD) {
		// Just pass IRP
		IoCopyCurrentIrpStackLocationToNext(Irp);
		return IoCallDriver(((PDEVICE_EXTENSION)DeviceObject->DeviceExtension)->lowerKeyboardExtension, Irp);
	}

	Irp->IoStatus.Information = 0;
	Irp->IoStatus.Status = STATUS_SUCCESS;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	return STATUS_SUCCESS;
}

NTSTATUS ReadKeys(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	PKEYBOARD_INPUT_DATA keys = (PKEYBOARD_INPUT_DATA)Irp->AssociatedIrp.SystemBuffer;

	int keysData = Irp->IoStatus.Information / sizeof(KEYBOARD_INPUT_DATA);
	if (Irp->IoStatus.Status == STATUS_SUCCESS)
	{
		for (int i = 0; i < keysData; i++) {
			DbgPrint("kbScanner: Scan code is %x\n", keys[i].MakeCode);
		}
	}

	if (Irp->PendingReturned) {
		IoMarkIrpPending(Irp);
	}

	pendingIrp--;
	return Irp->IoStatus.Status;
}

NTSTATUS DispatchRead(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	IoCopyCurrentIrpStackLocationToNext(Irp);
	IoSetCompletionRoutine(Irp, (PIO_COMPLETION_ROUTINE)ReadKeys, NULL, TRUE, TRUE, TRUE);

	pendingIrp++;
	return IoCallDriver(((PDEVICE_EXTENSION)DeviceObject->DeviceExtension)->lowerKeyboardExtension, Irp);
}

NTSTATUS DispatchControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(Irp);
	NTSTATUS status = STATUS_SUCCESS;

	PVOID buffer = Irp->AssociatedIrp.SystemBuffer;
	ULONG inLength = irpStack->Parameters.DeviceIoControl.InputBufferLength;
	ULONG outLength = irpStack->Parameters.DeviceIoControl.OutputBufferLength;

	ULONG message = 0;
	WCHAR* keyboardData = L"0001";

	if (irpStack->Parameters.DeviceIoControl.IoControlCode == DEVICE_READ) {
		wcsncpy(buffer, keyboardData, 63);
		message = (wcsnlen(buffer, 63) + 1) * 2;
	}
	else {
		status = STATUS_INVALID_PARAMETER;
	}

	Irp->IoStatus.Status = status;
	Irp->IoStatus.Information = message;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	return status;
}


NTSTATUS MyAttachDevice(PDRIVER_OBJECT DriverObject)
{
	UNICODE_STRING targetDevice = RTL_CONSTANT_STRING(L"\\Device\\KeyboardClass0");

	NTSTATUS status;
	status = IoCreateDevice(DriverObject, sizeof(DEVICE_EXTENSION), NULL, FILE_DEVICE_KEYBOARD, 0, FALSE, &keyboardExtension);

	if (!NT_SUCCESS(status)) {
		return status;
	}

	// This extension will ONLY read existing keyboard buffer
	// Do not initialize keyboard again!
	keyboardExtension->Flags |= DO_BUFFERED_IO;
	keyboardExtension->Flags &= ~DO_DEVICE_INITIALIZING;

	// Reset memory of this extension
	RtlZeroMemory(keyboardExtension->DeviceExtension, sizeof(DEVICE_EXTENSION));

	// Attach device extension to keyboard
	IoAttachDevice(
		keyboardExtension, 
		&targetDevice, 
		&((PDEVICE_EXTENSION)keyboardExtension->DeviceExtension)->lowerKeyboardExtension
	);

	if (!NT_SUCCESS(status)) {
		IoDeleteDevice(keyboardExtension);
		return status;
	}

	return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{	
	NTSTATUS status;
	DriverObject->DriverUnload = DriverUnload;

	// Create new device for communication with our app
	status = IoCreateDevice(DriverObject, 0, &DeviceName, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE, &DeviceObject);

	if (!NT_SUCCESS(status)) {
		DbgPrint("kbScanner: Failed to create device\n");
		return status;
	}
	else {
		DbgPrint("kbScanner: Device successfully initialized\n");
	}

	status = IoCreateSymbolicLink(&SymLinkName, &DeviceName);

	if (!NT_SUCCESS(status)) {
		DbgPrint("kbScanner: Failed creating symbolic link\n");
		return status;
	}
	else {
		DbgPrint("kbScanner: Symbolic link successfully created\n");
	}

	for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
		DriverObject->MajorFunction[i] = DispatchPass;
	}

	// This request is sent by I/O manager
	// For instance when keyboard or mouse key is clicked
	DriverObject->MajorFunction[IRP_MJ_READ] = DispatchRead;

	// Respond to device control
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchControl;

	status = MyAttachDevice(DriverObject);
	if (!NT_SUCCESS(status)) {
		DbgPrint("kbScanner: Failed to attach device\n");
		return status;
	}
	else {
		DbgPrint("kbScanner: Driver successfully attached to keyboard\n");
	}

	return STATUS_SUCCESS;
}