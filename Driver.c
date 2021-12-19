#include <ntddk.h>

UNICODE_STRING DeviceName = RTL_CONSTANT_STRING(L"\\Device\\KeyboardScanner");

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
	// Just pass IRP
	IoCopyCurrentIrpStackLocationToNext(Irp);
	return IoCallDriver(((PDEVICE_EXTENSION)DeviceObject->DeviceExtension)->lowerKeyboardExtension, Irp);
}

VOID moveMouse(USHORT code)
{
	PUCHAR signalPortPointer = (PUCHAR)0x64;
	PUCHAR mousePortPointer = (PUCHAR)0x60;

	UCHAR startSignal = 0xD3;
	UCHAR flagByte = 0x08;

	UCHAR moveX = 0;
	UCHAR moveY = 0;

	UCHAR step = 1;
	DbgPrint("kbScanner: Scan code %d\n", code);
	
	switch (code) {
	case 72:	// UP
		moveX++;
		moveX *= step;
		break;

	case 80:	// DOWN
		moveY++;
		moveY *= step;
		break;

	case 75:
		flagByte |= 0x10;
		moveX = -moveX;
		break;

	case 77:
		flagByte |= 0x20;
		moveY = -moveY;
		break;

	default:
		break;
	}

	WRITE_PORT_UCHAR(signalPortPointer, startSignal);
	WRITE_PORT_UCHAR(mousePortPointer, flagByte);
	WRITE_PORT_UCHAR(signalPortPointer, startSignal);
	WRITE_PORT_UCHAR(mousePortPointer, moveX);
	WRITE_PORT_UCHAR(signalPortPointer, startSignal);
	WRITE_PORT_UCHAR(mousePortPointer, moveY);
}

NTSTATUS ReadKeys(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	PKEYBOARD_INPUT_DATA keys = (PKEYBOARD_INPUT_DATA)Irp->AssociatedIrp.SystemBuffer;
	int keysData = Irp->IoStatus.Information / sizeof(KEYBOARD_INPUT_DATA);

	if (Irp->IoStatus.Status == STATUS_SUCCESS)
	{
		for (int i = 0; i < keysData; i++) {
			// Arrows are SPECIAL, so they are assigned to so called "special keyboard functions"
			// Flag 3 indicates that the key is pressed
			// Flag 4 that is just released
			if (keys[i].Flags == 3) {
				moveMouse(keys[i].MakeCode);
			}
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

	for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
		DriverObject->MajorFunction[i] = DispatchPass;
	}

	// This request is sent by I/O manager
	// For instance when keyboard or mouse key is clicked
	DriverObject->MajorFunction[IRP_MJ_READ] = DispatchRead;

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