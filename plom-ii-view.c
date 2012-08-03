#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int main (int argc, char *argv[]) {

  // Try to initialize map from command line arguments.
  FILE * file;
  if (argc != 2 && argc != 3 ||
      !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
    printf("IRC viewer.\n");
    exit(0); }
  else if (argc == 2) {
    file = fopen(argv[1], "r"); }
  else if (argc == 3) {
    printf("In-files not yet implemented.\n");
    exit(0); }

  // Initialize screen.
  WINDOW * screen = initscr();
  curs_set(0);
  timeout(10);
  keypad(screen, TRUE);
  noecho();
  int rows, cols;
  getmaxyx(screen, rows, cols);

  int y, x, l, key, start = 0;
  char line[1024], end = 0;
  while (1) {

    fseek(file, start, 0);
    for (y = 0; y < rows; y++) {
      if (fgets(line, 1024, file))
        l = strlen(line);
      else {
        l = 0;
        end = 1; }
      if (l && y == rows - 1)
        end = 0;
      for (x = 0; x < cols; x++)
        if (x < l)
          mvaddch(y, x, line[x]);
        else
          mvaddch(y, x, ' '); }
    refresh();

    key = getch();

    if (!key)
      continue;
    else if (key == 'q')
       break;

    else if (key == KEY_UP || key == KEY_PPAGE) {
      l = 1;
      for (x = start - 2; ; x--) {
        if (x <= 0) {
          start = 0;
          break; }
        fseek(file, x, 0);
        y = (char) fgetc(file);
        if (y == '\n') {
          if (key == KEY_UP || l == rows) {
            start = x + 1;
            break; }
          l++; } } }

    else if (key == KEY_DOWN && !end) {
      fseek(file, start, 0);
      fgets(line, 1024, file);
      start = ftell(file); }
    else if (key == KEY_NPAGE && !end) {
      l = start;
      for (x = 0; x < rows; x++) {
        fseek(file, l, 0);
        for (y = 0; y < rows; y++) {
          if (y == 1)
            l = ftell(file);
          if (!fgets(line, 1024, file)) {
            y = 0;
            break; } }
        if (!y)
          break;
        start = l; } } }

  endwin();
  exit(0); }
