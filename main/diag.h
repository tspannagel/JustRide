// Bench diagnostics on the USB console.
#pragma once

// Start the REPL with the diag commands (i2c, pattern, sd, buzz, mem).
void diag_start(void);

// Draw test pattern 0 (colour bars), 1 (white) or 2 (black).
void diag_test_pattern(int which);
