// Bridging header: C declarations made visible to Swift (passed to swiftc with -import-objc-header).
//
// IOAVService is a private IOKit interface that Apple Silicon Macs use to talk to the display
// controller (DCP). It is not in the public SDK headers, but the symbols are exported by IOKit, so
// declaring them here is enough to link. It is how DDC/CI reaches external monitors on Apple Silicon
// (Intel Macs used the public IOI2C* API instead); tools such as m1ddc and MonitorControl rely on it too.

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

typedef CFTypeRef IOAVServiceRef;

/// Wraps a DCPAVServiceProxy registry entry in an IOAVService object (returned retained, or NULL).
extern IOAVServiceRef IOAVServiceCreateWithService(CFAllocatorRef allocator, io_service_t service);

/// Writes `inputBufferSize` bytes on the display's I²C (DDC) bus.
/// `chipAddress` is the 7-bit I²C address (0x37 for DDC/CI); `dataAddress` is sent as the first byte
/// of the transfer, which in DDC/CI is the source address of the host (0x51).
extern IOReturn IOAVServiceWriteI2C(IOAVServiceRef service, uint32_t chipAddress, uint32_t dataAddress,
                                    void *inputBuffer, uint32_t inputBufferSize);
