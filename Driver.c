#include <ntddk.h>

UNICODE_STRING DeviceName = RTL_CONSTANT_STRING(L"\\Device\\KeyboardScanner");

typedef struct {
	PDEVICE_OBJECT lowerDevice;
} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

// Keyboard Extension
PDEVICE_OBJECT keyboardExtension = NULL;

typedef struct _KEYBOARD_INPUT_DATA {
	USHORT UnitId;
	USHORT MakeCode;
	USHORT Flags;
	USHORT Reserved;
	ULONG  ExtraInformation;
} KEYBOARD_INPUT_DATA, * PKEYBOARD_INPUT_DATA;

// Mouse Extension
PDEVICE_OBJECT mouseExtension = NULL;

typedef struct _MOUSE_INPUT_DATA {
	USHORT UnitId;
	USHORT Flags;
	union {
		ULONG Buttons;
		struct {
			USHORT ButtonFlags;
			USHORT ButtonData;
		};
	};
	ULONG  RawButtons;
	LONG   LastX;
	LONG   LastY;
	ULONG  ExtraInformation;
} MOUSE_INPUT_DATA, * PMOUSE_INPUT_DATA;

ULONG pendingIrp = 0;


VOID DriverUnload(PDRIVER_OBJECT DriverObject)
{
	// Detach keyboard
	PDEVICE_OBJECT DeviceObject = DriverObject->DeviceObject;
	IoDetachDevice(
		((PDEVICE_EXTENSION)DeviceObject->DeviceExtension)->lowerDevice
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
	return IoCallDriver(((PDEVICE_EXTENSION)DeviceObject->DeviceExtension)->lowerDevice, Irp);
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
	return IoCallDriver(((PDEVICE_EXTENSION)DeviceObject->DeviceExtension)->lowerDevice, Irp);
}

NTSTATUS MoveMouse(PDEVICE_OBJECT DeviceObject, ULONG IoctlControlCode)
{
	KEVENT              event;
	PIRP                irp;
	IO_STATUS_BLOCK     ioStatus;
	NTSTATUS status;

	char OutBuf[1024];
	ULONG OutBufLen = 1024;
	char InBuf[1024];
	ULONG InBufLen = 1024;

	KeInitializeEvent(&event, NotificationEvent, FALSE);

	if (NULL == irp) {
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	status = IoCallDriver(((PDEVICE_EXTENSION)DeviceObject->DeviceExtension)->lowerDevice, irp);

	if (status == STATUS_PENDING) {
		// 
		// You must wait here for the IRP to be completed because:
		// 1) The IoBuildDeviceIoControlRequest associates the IRP with the
		//     thread and if the thread exits for any reason, it would cause the IRP
		//     to be canceled. 
		// 2) The Event and IoStatus block memory is from the stack and we
		//     cannot go out of scope.
		// This event will be signaled by the I/O manager when the
		// IRP is completed.
		// 
		status = KeWaitForSingleObject(
			&event,
			Executive, // wait reason
			KernelMode, // To prevent stack from being paged out.
			FALSE,     // You are not alertable
			NULL);     // No time out !!!!

		status = ioStatus.Status;
	}

	return status;
}

/*	IoCopyCurrentIrpStackLocationToNext(Irp);
	IoSetCompletionRoutine(Irp, (PIO_COMPLETION_ROUTINE)ReadKeys, NULL, TRUE, TRUE, TRUE);

	pendingIrp++;
	return IoCallDriver(((PDEVICE_EXTENSION)DeviceObject->DeviceExtension)->lowerDevice, Irp);
}*/

NTSTATUS MyAttachDevices(PDRIVER_OBJECT DriverObject)
{
	UNICODE_STRING keyboardDevice = RTL_CONSTANT_STRING(L"\\Device\\KeyboardClass0");
	UNICODE_STRING mouseDevice = RTL_CONSTANT_STRING(L"\\Device\\PointerClass0");
	NTSTATUS status;

	status = IoCreateDevice(DriverObject, sizeof(DEVICE_EXTENSION), NULL, FILE_DEVICE_KEYBOARD, 0, FALSE, &keyboardExtension);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	status = IoCreateDevice(DriverObject, sizeof(DEVICE_EXTENSION), NULL, FILE_DEVICE_MOUSE, 0, FALSE, &mouseExtension);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	// Use buffered IO for better performance
	// Signal that our driver is about to start
	keyboardExtension->Flags |= DO_BUFFERED_IO;				
	keyboardExtension->Flags &= ~DO_DEVICE_INITIALIZING;

	// Reset memory of this extension
	RtlZeroMemory(keyboardExtension->DeviceExtension, sizeof(DEVICE_EXTENSION));

	// Attach device extension to keyboard
	IoAttachDevice(
		keyboardExtension, 
		&keyboardDevice,
		&((PDEVICE_EXTENSION)keyboardExtension->DeviceExtension)->lowerDevice
	);
	if (!NT_SUCCESS(status)) {
		IoDeleteDevice(keyboardExtension);
		return status;
	}

	mouseExtension->Flags |= DO_BUFFERED_IO;
	mouseExtension->Flags &= ~DO_DEVICE_INITIALIZING;

	// Attach device extension to mouse
	IoAttachDevice(
		mouseExtension,
		&mouseDevice,
		&((PDEVICE_EXTENSION)mouseExtension->DeviceExtension)->lowerDevice
	);
	if (!NT_SUCCESS(status)) {
		IoDeleteDevice(mouseExtension);
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
	// For instance when keyboard key is clicked
	DriverObject->MajorFunction[IRP_MJ_READ] = DispatchRead;

	// For instance when keyboard key is clicked
	DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = MoveMouse;

	status = MyAttachDevices(DriverObject);
	if (!NT_SUCCESS(status)) {
		DbgPrint("kbScanner: Failed to attach device\n");
		return status;
	}
	else {
		DbgPrint("kbScanner: Driver successfully attached to keyboard\n");
	}

	return STATUS_SUCCESS;
}