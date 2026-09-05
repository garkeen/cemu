#include "util/table.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug/debug.h"
#include "host/host.h"

// One line buffer per table; a row is assembled here and flushed once,
// keeping writes line-atomic on the console.
struct Table {
  const TableColumn* cols;
  int ncols;
  int ascii;
  int header_every;
  int rows;
  char line[1024];
  int len;
  int cell;  // cells filled in the row being assembled
};

// Glyphs. UTF-8 box drawing by default, plain ASCII with `ascii`.
static const char* Vertical(Table* t) { return t->ascii ? "|" : "\xe2\x94\x82"; }
static const char* Cross(Table* t) { return t->ascii ? "+" : "\xe2\x94\xbc"; }
static const char* Horz(Table* t) { return t->ascii ? "-" : "\xe2\x94\x80"; }

static void Put(Table* t, const char* s) {
  int n = (int)strlen(s);
  if (t->len + n + 1 >= (int)sizeof(t->line)) return;
  memcpy(t->line + t->len, s, (size_t)n);
  t->len += n;
}

// Append a run of n horizontal-rule glyphs.
static void PutRule(Table* t, int n) {
  for (int i = 0; i < n; i++) Put(t, Horz(t));
}

Table* TableOpen(const TableColumn* cols, int ncols, int ascii, int header_every) {
  Table* t = calloc(1, sizeof(Table));
  t->cols = cols;
  t->ncols = ncols;
  t->ascii = ascii;
  t->header_every = header_every;
  return t;
}

void TableFree(Table* t) { free(t); }

static void Flush(Table* t) {
  // Gate the write through the session output cap: once the cap is hit, drop
  // further rows so a misconfigured CEMU_DEBUG can't fill the disk.
  if (t->len && DebugAccountOut((size_t)t->len))
    HostWriteErr(t->line, (size_t)t->len);
  t->len = 0;
}

void TableHeader(Table* t) {
  // separator rule, then the title row
  t->len = 0;
  Put(t, Vertical(t));
  for (int c = 0; c < t->ncols; c++) {
    PutRule(t, t->cols[c].width + 1);  // +1: one blank padding column
    Put(t, Cross(t));
  }
  // The line ends with the final cross replacing a trailing vertical; back
  // up one glyph length and close cleanly.
  t->len -= (int)strlen(Vertical(t));
  Put(t, "\n");
  Flush(t);

  t->len = 0;
  Put(t, Vertical(t));
  for (int c = 0; c < t->ncols; c++) {
    char cell[64];
    snprintf(cell, sizeof(cell), " %-*s", t->cols[c].width, t->cols[c].title);
    Put(t, cell);
    Put(t, Vertical(t));
  }
  Put(t, "\n");
  Flush(t);

  t->len = 0;
  Put(t, Vertical(t));
  for (int c = 0; c < t->ncols; c++) {
    PutRule(t, t->cols[c].width + 1);
    Put(t, Cross(t));
  }
  t->len -= (int)strlen(Vertical(t));
  Put(t, "\n");
  Flush(t);
}

void TableRowBegin(Table* t) {
  // Re-print the header periodically so long greppable dumps stay readable.
  if (t->header_every && t->rows && t->rows % t->header_every == 0) TableHeader(t);
  t->len = 0;
  Put(t, Vertical(t));
  t->cell = 0;
}

void TableRowCell(Table* t, const char* text) {
  if (t->cell >= t->ncols) return;
  const TableColumn* c = &t->cols[t->cell];
  // Truncate over-wide cells: the fixed width is the diff-stability contract.
  char cell[256];
  snprintf(cell, sizeof(cell), c->align == kTableRight ? " %*s" : " %-*s", c->width, text);
  cell[c->width + 1] = 0;  // snprintf pads to width but never truncates
  Put(t, cell);
  Put(t, Vertical(t));
  t->cell++;
}

void TableRowEnd(Table* t) {
  Put(t, "\n");
  Flush(t);
  t->rows++;
}
