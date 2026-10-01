#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

// Initialize/Open are not imported: no selection dialog is ever opened, nothing to tear down.
int APS5_VABI scePlayerSelectionDialogTerminate(void) {
 return 0;
}

}
