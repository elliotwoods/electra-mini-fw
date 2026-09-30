/* macOS vendor-bulk transport. Interface ownership stays on the provider worker. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct { IOUSBInterfaceInterface300 **usb; UInt8 in_pipe, out_pipe; } ElectraUSB;

static int property(io_service_t service, CFStringRef key, int expected) {
    CFTypeRef value = IORegistryEntrySearchCFProperty(service, kIOServicePlane, key,
        kCFAllocatorDefault, kIORegistryIterateRecursively | kIORegistryIterateParents);
    int number = -1;
    if (value && CFGetTypeID(value) == CFNumberGetTypeID())
        CFNumberGetValue(value, kCFNumberIntType, &number);
    if (value) CFRelease(value);
    return number == expected;
}
int electra_usb_list(uint64_t *ids, unsigned capacity, unsigned *count) {
    io_iterator_t iterator = 0;
    IOReturn result = IOServiceGetMatchingServices(kIOMainPortDefault,
        IOServiceMatching(kIOUSBInterfaceClassName), &iterator);
    if (result) return result;
    *count = 0;
    io_service_t service;
    while ((service = IOIteratorNext(iterator))) {
        if (property(service, CFSTR("idVendor"), 0x1fc9) &&
            property(service, CFSTR("idProduct"), 0x82d0) &&
            property(service, CFSTR("bInterfaceClass"), 0xff) && *count < capacity) {
            uint64_t id;
            if (!IORegistryEntryGetRegistryEntryID(service, &id)) ids[(*count)++] = id;
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    return 0;
}
void electra_usb_close(ElectraUSB *connection) {
    if (!connection) return;
    if (connection->usb) {
        (*connection->usb)->USBInterfaceClose(connection->usb);
        (*connection->usb)->Release(connection->usb);
    }
    free(connection);
}
int electra_usb_open(uint64_t id, ElectraUSB **out) {
    *out = NULL;
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IORegistryEntryIDMatching(id));
    if (!service) return kIOReturnNoDevice;
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    IOReturn result = IOCreatePlugInInterfaceForService(service, kIOUSBInterfaceUserClientTypeID,
        kIOCFPlugInInterfaceID, &plugin, &score);
    IOObjectRelease(service);
    if (result) return result;
    IOUSBInterfaceInterface300 **usb = NULL;
    HRESULT query = (*plugin)->QueryInterface(plugin,
        CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID300), (LPVOID *)&usb);
    IODestroyPlugInInterface(plugin);
    if (query || !usb) return kIOReturnUnsupported;
    result = (*usb)->USBInterfaceOpen(usb);
    if (result) { (*usb)->Release(usb); return result; }
    ElectraUSB *connection = calloc(1, sizeof(*connection));
    if (!connection) { (*usb)->USBInterfaceClose(usb); (*usb)->Release(usb); return kIOReturnNoMemory; }
    connection->usb = usb;
    UInt8 endpoints = 0;
    result = (*usb)->GetNumEndpoints(usb, &endpoints);
    for (UInt8 pipe = 1; !result && pipe <= endpoints; ++pipe) {
        UInt8 direction, number, transfer, interval;
        UInt16 packet;
        result = (*usb)->GetPipeProperties(usb, pipe, &direction, &number, &transfer, &packet, &interval);
        if (!result && transfer == kUSBBulk && number == 1) {
            if (direction == kUSBIn) connection->in_pipe = pipe;
            else connection->out_pipe = pipe;
        }
    }
    if (result || !connection->in_pipe || !connection->out_pipe) {
        electra_usb_close(connection);
        return result ? result : kIOReturnUnsupported;
    }
    *out = connection;
    return 0;
}
int electra_usb_write(ElectraUSB *c, const void *bytes, uint32_t size) {
    return (*c->usb)->WritePipeTO(c->usb, c->out_pipe, (void *)bytes, size, 1000, 1000);
}
int electra_usb_read(ElectraUSB *c, void *bytes, uint32_t *size, uint32_t timeout) {
    /* One full-speed packet per transfer: IOKit does not report partial progress
       on a timed-out multi-packet read. Never discard consumed stream bytes. */
    uint32_t capacity = *size, total = 0;
    while (total < capacity) {
        UInt32 n = capacity - total < 64 ? capacity - total : 64;
        IOReturn result = (*c->usb)->ReadPipeTO(c->usb, c->in_pipe,
            (uint8_t *)bytes + total, &n, total ? 1 : timeout, total ? 1 : timeout);
        if (result == kIOUSBTransactionTimeout || result == kIOReturnTimeout) break;
        if (result) { *size = total; return result; }
        total += n;
        if (n < 64) break;
    }
    *size = total;
    return 0;
}
