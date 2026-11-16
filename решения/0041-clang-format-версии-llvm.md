# 0041. clang-format той же версии, что LLVM

- Статус: принято
- Версия: v0.050.13
- Спецификация: `.clang-format`; `cmake/llvm.cmake`; задача `style` в CI

## Решение

Стиль C++ проверяет clang-format версии из `cmake/llvm.cmake` — пакет `clang-format` из PyPI. Стиль — Allman, 4 пробела, 120 столбцов.

## Причина

Разные версии clang-format форматируют один файл по-разному: локальный формат и CI разошлись бы.

## Отвергнутые варианты

- clang-format из образа CI: в Ubuntu 24.04 — версия 18.
