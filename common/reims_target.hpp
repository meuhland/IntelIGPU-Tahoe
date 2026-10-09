#pragma once
#include <IOKit/pci/IOPCIDevice.h>

// Physical GPU identity verified by every kernel component. Build scripts
// select the target with REIMS_TARGET (see scripts/target-flags.sh); the
// default remains the original ADL-P laptop so upstream builds are unchanged.
// REIMS_REVISION_B64 is the registry revision-id (4 bytes, little endian)
// spliced into manual-gate/profiles.hpp; keep it in sync with kRevision.
namespace ReimsTarget {
constexpr uint16_t kVendor=0x8086;
#if defined(REIMS_TARGET_RPLS)
// Raptor Lake-S GT1, UHD Graphics 770.
constexpr uint16_t kDevice=0xa780;
constexpr uint8_t kRevision=0x04;
#define REIMS_REVISION_B64 "BAAAAA=="
// Desktop: no internal panel or PCH backlight PWM.
constexpr bool kInternalPanel=false;
// Display takeover and the ADL-P RCS PSMI workaround write ADL-P registers.
// Keep them refused until their RPL-S paths are ported; read-only probes stay.
constexpr bool kHardwareWritesPorted=false;
// Proxmox/QEMU q35 guest whose SSDT names the passed-through IGD GFX0 at
// 00:02.0. Unverified: confirm on the guest and override with REIMS_PCI_PATH.
// A mismatch fails closed (the gate refuses the device).
#define REIMS_DEFAULT_PCI_PATH "IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/GFX0@2"
#else
// Alder Lake-P, the validated upstream machine.
constexpr uint16_t kDevice=0x46a3;
constexpr uint8_t kRevision=0x0c;
#define REIMS_REVISION_B64 "DAAAAA=="
constexpr bool kInternalPanel=true;
constexpr bool kHardwareWritesPorted=true;
#define REIMS_DEFAULT_PCI_PATH "IOService:/AppleACPIPlatformExpert/PC00/AppleACPIPCI/GFX0@2"
#endif
#ifndef REIMS_PCI_PATH
#define REIMS_PCI_PATH REIMS_DEFAULT_PCI_PATH
#endif
constexpr const char *kPCIPath=REIMS_PCI_PATH;

// Vendor and device only, for checks that upstream made without a revision.
inline bool isDevice(IOPCIDevice *pci){
 return pci&&pci->configRead16(kIOPCIConfigVendorID)==kVendor&&
        pci->configRead16(kIOPCIConfigDeviceID)==kDevice;
}
inline bool isExact(IOPCIDevice *pci){
 return isDevice(pci)&&pci->configRead8(kIOPCIConfigRevisionID)==kRevision;
}
}
