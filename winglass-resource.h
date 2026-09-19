// Shared resource identifiers.
// Kept in a separate header so both the resource script (winglass.rc) and the
// C++ sources refer to the same numeric IDs.

#ifndef WINGLASS_RESOURCE_H
#define WINGLASS_RESOURCE_H

#define IDI_WINGLASS_ICON 101

// Version reported by all three executables.
//
// Keep this in sync with the released git tag.  Only the numbers live here;
// winglass.rc turns them into the VERSIONINFO block and fills in the
// per-executable FileDescription / OriginalFilename.
//
// WINGLASS_FILEVERSION is a bare comma list, because the resource script's
// FILEVERSION and PRODUCTVERSION statements want `1,2,0,0` and cannot build it
// from separate numbers.  WINGLASS_FILEVERSION_STRING is the same value in
// dotted form for the string table.  The resource compiler's preprocessor has
// no stringify operator, so these two are updated by hand - change both.
#define WINGLASS_VERSION_MAJOR 1
#define WINGLASS_VERSION_MINOR 2
#define WINGLASS_VERSION_PATCH 0
#define WINGLASS_VERSION_BUILD 0

#define WINGLASS_FILEVERSION WINGLASS_VERSION_MAJOR,WINGLASS_VERSION_MINOR,WINGLASS_VERSION_PATCH,WINGLASS_VERSION_BUILD

#define WINGLASS_FILEVERSION_STRING "1.2.0.0"
#define WINGLASS_PRODUCTVERSION_STRING "1.2.0"

#define WINGLASS_PRODUCT_NAME "WinGlass"
#define WINGLASS_COMPANY_NAME "Akagi_0612"
#define WINGLASS_COPYRIGHT "Copyright (c) 2026 Akagi_0612"

#endif  // WINGLASS_RESOURCE_H
