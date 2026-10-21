// Stand-in for Xenia's base/logging.h. Vendored code logs through Xenia's own formatter, which
// accepts arguments fmt cannot (enums, bit-fields), so these messages are dropped.
#pragma once
#define XELOGE(...) ((void)0)
#define XELOGW(...) ((void)0)
#define XELOGI(...) ((void)0)
#define XELOGD(...) ((void)0)
#define XELOGGPU(...) ((void)0)
#define XELOGAPU(...) ((void)0)
