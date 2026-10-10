#pragma once

// Append a timestamped line to trainer/trainer.log (flushed every line, so a crash keeps the tail).
void tlog(const char* fmt, ...);
void tlog_open(const char* path);
