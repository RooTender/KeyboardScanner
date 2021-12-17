#include <ntddk.h>

typedef struct {
	PDEVICE_OBJECT lowerKeyboardExtension;
} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

PDEVICE_OBJECT keyboardExtension = NULL;


VOID DriverUnload(PDRIVER_OBJECT DriverObject)
{
	PDEVICE_OBJECT DeviceObject = DriverObject->DeviceObject;
	IoDetachDevice(((PDEVICE_EXTENSION)DeviceObject->DeviceExtension)->lowerKeyboardExtension);
	IoDeleteDevice(keyboardExtension);

	DbgPrint("kbScanner: Driver unloaded\n");
}

NTSTATUS DispatchPass(PDEVICE_OBJECT DeviceObject, PIRP pirp)
{
	return STATUS_SUCCESS;
}

NTSTATUS DispatchRead(PDEVICE_OBJECT DeviceObject, PIRP pirp)
{
	return STATUS_SUCCESS;
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

	DriverObject->MajorFunction[IRP_MJ_READ] = DispatchRead;

	status = MyAttachDevice(DriverObject);
	if (!NT_SUCCESS(status)) {
		DbgPrint("kbScanner: Failed to attach device\n");
		return status;
	}
	else {
		DbgPrint("kbScanner: Successfully initialized\n");
	}

	return STATUS_SUCCESS;
}