#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

// Neither Initialize nor Open is imported: no invitation dialog is ever opened, so status is
// always "none" and there is nothing for Terminate to tear down.
int APS5_VABI scePlayerInvitationDialogTerminate(void) {
 return 0;
}

int APS5_VABI scePlayerInvitationDialogUpdateStatus(void) {
 return 0;
}

}
