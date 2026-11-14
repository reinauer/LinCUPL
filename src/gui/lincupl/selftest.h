#pragma once

namespace SelfTest {

// Runs headless verification of .dl parsing, Atmel.cat, CuplErr.txt,
// CUPL highlighting, wizards, QProcess compilation/simulation, output tabs,
// and diagnostic navigation. Returns 0 on success, non-zero on failure.
int run();

} // namespace SelfTest
