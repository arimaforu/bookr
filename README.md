# Bookr

Minimal terminal book reader for macOS/Linux.

![C++](https://img.shields.io/badge/C%2B%2B-17-blue)
![Build](https://img.shields.io/badge/build-CMake-green)
![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux-lightgrey)

---

## EN

### Overview

- Fullscreen terminal reader
- Folder-based library browser
- Whole-word search with highlight
- TOC support (where available)
- Reading position persistence

### Supported Formats

- `txt`
- `fb2`
- `epub`
- `pdf`

### Important Notes

- TOC:
- `epub`: metadata (`nav` / `NCX`)
- `fb2`: XML `section/title`
- `pdf` / `txt`: heading heuristics
- Footnotes:
- `epub`: supported (`[1]`, `[^1]`, superscript digits like `¹²`)
- `fb2`: temporarily unavailable
- `pdf` / `txt`: work when note lines exist in text

### Dependencies

- `fb2` / `epub`: `pandoc`
- `pdf`: `pdftotext` (from `poppler`)

### Install (macOS)

```bash
brew install pandoc poppler
```

### Install (Arch Linux)

```bash
sudo pacman -S pandoc poppler
```

### Build

```bash
cmake -S . -B build
cmake --build build
```

### Run

```bash
./build/bookr
./build/bookr /path/to/book.epub
```

### CLI

```bash
./build/bookr --help
./build/bookr --version
./build/bookr --add-folder /path/to/books
./build/bookr --remove-folder /path/to/books
./build/bookr --list-folders
```

### Reader Controls

- `->` / mouse wheel down: next page
- `<-` / mouse wheel up: previous page
- `/`: search
- `]` or `down`: next match
- `[` or `up`: previous match
- `t`: TOC
- `f`: footnotes
- `c`: copy paragraph
- `q`: quit

### Data Files

- Library folders: `~/.config/bookr/folders`
- Reading positions: `~/.config/bookr/state`

---

## RU

### Что это

- Полноэкранная читалка в терминале
- Библиотека книг по папкам
- Поиск по целому слову с подсветкой
- Оглавление (где доступно)
- Запоминание позиции чтения

### Поддерживаемые форматы

- `txt`
- `fb2`
- `epub`
- `pdf`

### Важные заметки

- TOC:
- `epub`: метаданные (`nav` / `NCX`)
- `fb2`: XML `section/title`
- `pdf` / `txt`: эвристика заголовков
- Сноски:
- `epub`: поддерживаются (`[1]`, `[^1]`, надстрочные цифры `¹²`)
- `fb2`: временно недоступны
- `pdf` / `txt`: работают, если строки сносок есть в тексте

### Зависимости

- `fb2` / `epub`: `pandoc`
- `pdf`: `pdftotext` (из `poppler`)

### Установка (macOS)

```bash
brew install pandoc poppler
```

### Установка (Arch Linux)

```bash
sudo pacman -S pandoc poppler
```

### Сборка

```bash
cmake -S . -B build
cmake --build build
```

### Запуск

```bash
./build/bookr
./build/bookr /path/to/book.epub
```

### CLI

```bash
./build/bookr --help
./build/bookr --version
./build/bookr --add-folder /path/to/books
./build/bookr --remove-folder /path/to/books
./build/bookr --list-folders
```

### Управление в читалке

- `->` / колесо вниз: следующая страница
- `<-` / колесо вверх: предыдущая страница
- `/`: поиск
- `]` или `down`: следующее совпадение
- `[` или `up`: предыдущее совпадение
- `t`: оглавление
- `f`: сноски
- `c`: копировать абзац
- `q`: выход

### Файлы данных

- Папки библиотеки: `~/.config/bookr/folders`
- Позиции чтения: `~/.config/bookr/state`

## License

This project is licensed under the [MIT License](LICENSE).
