// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#pragma once

#include <string>

namespace cdrip::terminal {

/**
 * Query Kitty graphics support once on the CLI's input/output terminal.
 * @return True if the terminal accepts direct image transmission.
 * @remarks Called on the CLI thread. Redirected or different input/output
 * terminals cannot be queried. Input received during the query is retained.
 */
bool probe_kitty_graphics();

/**
 * Read a console line, preserving input received during capability detection.
 * @param line Receives the line without its newline.
 * @return True when a line was read, including a final line ending at EOF.
 * @remarks Terminal replies are excluded from subsequent console input.
 */
bool read_console_line(std::string& line);

/** @return Whether capability detection has buffered a complete console line. */
bool has_buffered_console_input();

}
