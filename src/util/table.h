#ifndef CEMU_UTIL_TABLE_H
#define CEMU_UTIL_TABLE_H

// Streaming table renderer for the debug facility (AGENTS.md §X). Columns
// are declared with fixed widths; rows are formatted cell by cell and written
// immediately — no full-table buffering, so million-line traces stay ordered
// (fprintf buffering scrambled event order once; everything goes through
// HostWriteErr here). Pure formatting: no host window APIs, no ISA knowledge.

typedef enum { kTableLeft = 0, kTableRight } TableAlign;

typedef struct TableColumn {
  const char* title;
  int width;  // declared fixed width: truncate wide cells, pad short
  TableAlign align;
} TableColumn;

typedef struct Table Table;

Table* TableOpen(const TableColumn* cols, int ncols, int ascii, int header_every);
// Prints the header rule and the title row. Re-prints both every
// header_every rows (0 = never again after the first).
void TableHeader(Table* t);
void TableRowBegin(Table* t);
// Cells must be pushed in column order; each call formats one cell.
void TableRowCell(Table* t, const char* text);
void TableRowEnd(Table* t);
void TableFree(Table* t);

#endif
