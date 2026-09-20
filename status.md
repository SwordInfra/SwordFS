# SwordFS POSIX conformance status

Latest run: **PASS**

| Metric | Value |
| --- | ---: |
| Overall conformance support | 57.16% |
| Supported regression pass | 100.00% |
| Classified rate | 100.00% |
| Supported assertion count | 5013 |
| Blocking result count | 0 |

## Classification counts

| Classification | Count |
| --- | ---: |
| KNOWN_SEMANTIC_DEFECT | 359 |
| KNOWN_UNSUPPORTED | 3398 |
| PASS | 5013 |
| UPSTREAM_NOT_APPLICABLE | 28 |

## Per-category support

| Category | PASS | Applicable | Support |
| --- | ---: | ---: | ---: |
| chflags | 14 | 14 | 100.00% |
| chmod | 184 | 327 | 56.27% |
| chown | 582 | 1470 | 39.59% |
| ftruncate | 83 | 89 | 93.26% |
| granular | 7 | 7 | 100.00% |
| link | 194 | 359 | 54.04% |
| mkdir | 95 | 118 | 80.51% |
| mkfifo | 48 | 120 | 40.00% |
| mknod | 60 | 186 | 32.26% |
| open | 235 | 337 | 69.73% |
| posix_fallocate | 1 | 1 | 100.00% |
| rename | 2880 | 4857 | 59.30% |
| rmdir | 125 | 145 | 86.21% |
| symlink | 87 | 95 | 91.58% |
| truncate | 77 | 84 | 91.67% |
| unlink | 246 | 439 | 56.04% |
| utimensat | 95 | 122 | 77.87% |

## Known-gap Issues

| Issue | Classification | Assertions | Reason |
| --- | --- | ---: | --- |
| [#208](https://github.com/SwordInfra/SwordFS/issues/208) | KNOWN_UNSUPPORTED | 104 | dependent assertion after unsupported special-file/FIFO/socket setup |
| [#208](https://github.com/SwordInfra/SwordFS/issues/208) | KNOWN_UNSUPPORTED | 3294 | special-file/FIFO/socket node semantics are not implemented |
| [#210](https://github.com/SwordInfra/SwordFS/issues/210) | KNOWN_SEMANTIC_DEFECT | 83 | permission, ownership, or sticky-directory authorization differs from POSIX |
| [#211](https://github.com/SwordInfra/SwordFS/issues/211) | KNOWN_SEMANTIC_DEFECT | 9 | overlong path components must return ENAMETOOLONG consistently |
| [#213](https://github.com/SwordInfra/SwordFS/issues/213) | KNOWN_SEMANTIC_DEFECT | 42 | pathname resolution or namespace error/transition semantics differ from POSIX |
| [#215](https://github.com/SwordInfra/SwordFS/issues/215) | KNOWN_SEMANTIC_DEFECT | 2 | truncate, file-size, or timestamp semantics differ from POSIX |
| [#217](https://github.com/SwordInfra/SwordFS/issues/217) | KNOWN_SEMANTIC_DEFECT | 223 | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |

## Known-gap assertions

| Assertion | Classification | Issue | Reason |
| --- | --- | --- | --- |
| `tests/chmod/00.t#22` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#23` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#24` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#26` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/00.t#27` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/00.t#28` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#31` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#32` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#33` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#34` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#36` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/00.t#37` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/00.t#38` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#41` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#42` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#43` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#44` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#46` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/00.t#47` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/00.t#48` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#51` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#52` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#53` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#54` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#56` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/00.t#57` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/00.t#58` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#61` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#70` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#71` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#72` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/00.t#73` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#74` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#75` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#76` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/00.t#77` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#78` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#79` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#80` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/00.t#81` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#82` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#83` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#84` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/00.t#85` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#87` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#88` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/00.t#91` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#92` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/00.t#94` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#95` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#96` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/00.t#97` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#98` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#99` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#100` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/00.t#101` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#102` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#103` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#104` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/00.t#105` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#106` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#107` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#108` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/00.t#109` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#117` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/01.t#5` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/01.t#6` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/01.t#7` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/01.t#8` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/01.t#9` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/01.t#10` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/01.t#11` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/01.t#12` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/01.t#13` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/01.t#14` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/01.t#15` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/01.t#16` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/02.t#5` | KNOWN_SEMANTIC_DEFECT | [#211](https://github.com/SwordInfra/SwordFS/issues/211) | overlong path components must return ENAMETOOLONG consistently |
| `tests/chmod/05.t#8` | KNOWN_SEMANTIC_DEFECT | [#210](https://github.com/SwordInfra/SwordFS/issues/210) | permission, ownership, or sticky-directory authorization differs from POSIX |
| `tests/chmod/07.t#7` | KNOWN_SEMANTIC_DEFECT | [#210](https://github.com/SwordInfra/SwordFS/issues/210) | permission, ownership, or sticky-directory authorization differs from POSIX |
| `tests/chmod/07.t#8` | KNOWN_SEMANTIC_DEFECT | [#210](https://github.com/SwordInfra/SwordFS/issues/210) | permission, ownership, or sticky-directory authorization differs from POSIX |
| `tests/chmod/07.t#10` | KNOWN_SEMANTIC_DEFECT | [#210](https://github.com/SwordInfra/SwordFS/issues/210) | permission, ownership, or sticky-directory authorization differs from POSIX |
| `tests/chmod/07.t#11` | KNOWN_SEMANTIC_DEFECT | [#210](https://github.com/SwordInfra/SwordFS/issues/210) | permission, ownership, or sticky-directory authorization differs from POSIX |
| `tests/chmod/07.t#17` | KNOWN_SEMANTIC_DEFECT | [#210](https://github.com/SwordInfra/SwordFS/issues/210) | permission, ownership, or sticky-directory authorization differs from POSIX |
| `tests/chmod/07.t#18` | KNOWN_SEMANTIC_DEFECT | [#210](https://github.com/SwordInfra/SwordFS/issues/210) | permission, ownership, or sticky-directory authorization differs from POSIX |
| `tests/chmod/07.t#20` | KNOWN_SEMANTIC_DEFECT | [#210](https://github.com/SwordInfra/SwordFS/issues/210) | permission, ownership, or sticky-directory authorization differs from POSIX |
| `tests/chmod/07.t#21` | KNOWN_SEMANTIC_DEFECT | [#210](https://github.com/SwordInfra/SwordFS/issues/210) | permission, ownership, or sticky-directory authorization differs from POSIX |
| `tests/chmod/11.t#18` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#19` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#20` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#22` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/11.t#23` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#25` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#26` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#27` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#28` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#30` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/11.t#31` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#33` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#34` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#35` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#36` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#38` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/11.t#39` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#41` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#42` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#43` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#44` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#46` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/11.t#47` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#49` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#69` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#70` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#71` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#73` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#74` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#75` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/11.t#76` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#78` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#79` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#80` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#81` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#83` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#84` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#85` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/11.t#86` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#88` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#89` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#90` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#91` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#93` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#94` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#95` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/11.t#96` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#98` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#99` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#100` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#101` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#103` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#104` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#105` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chmod/11.t#106` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#108` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#34` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#35` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#36` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#37` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#38` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#40` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chown/00.t#41` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chown/00.t#42` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#45` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#46` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#47` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#48` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#49` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#50` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#51` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#52` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#53` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#54` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#56` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chown/00.t#57` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chown/00.t#58` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#61` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#62` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#63` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#64` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#65` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#66` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#67` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#68` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#69` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#70` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#72` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chown/00.t#73` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chown/00.t#74` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#77` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#78` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#79` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#80` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#81` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#82` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#83` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#84` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#85` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#86` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#88` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chown/00.t#89` | KNOWN_SEMANTIC_DEFECT | [#217](https://github.com/SwordInfra/SwordFS/issues/217) | chmod/chown metadata mutation or mode/ownership side effects differ from POSIX |
| `tests/chown/00.t#90` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#93` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#94` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#95` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#96` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#97` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#162` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#163` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#164` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#165` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#166` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |

_Only the first 200 of 3757 known gaps are shown; see `result.json`._

## Baseline metadata

- SwordFS commit: `4ad8c24be0615b27f942707fd095362d4f6dd23b`
- pjdfstest commit: `85a8aea9e685999ef0540392fd80535f873d7ff7`
- kernel: `6.17.0-1022-azure`
- libfuse: `fusermount3 version: 3.18.2`
- os: `Linux-6.17.0-1022-azure-x86_64-with-glibc2.43`
- runner: `Linux`

Authoritative CI run: https://github.com/SwordInfra/SwordFS/actions/runs/35514475353
Recorded at: 2026-09-20T13:53:45+00:00

## Historical main-branch trend

| Time | SwordFS | Status | Overall support | Supported gate | Classified |
| --- | --- | --- | ---: | ---: | ---: |
| 2026-09-20T08:18:31+00:00 | `e844e9a8df64` | PASS | 57.14% | 100.00% | 100.00% |
| 2026-09-20T08:39:59+00:00 | `d330b30c7225` | PASS | 57.14% | 100.00% | 100.00% |
| 2026-09-20T08:51:31+00:00 | `0db6af791d79` | PASS | 57.14% | 100.00% | 100.00% |
| 2026-09-20T09:02:32+00:00 | `2400e6b0c49f` | PASS | 57.14% | 100.00% | 100.00% |
| 2026-09-20T09:36:44+00:00 | `e582a548952a` | PASS | 57.14% | 100.00% | 100.00% |
| 2026-09-20T09:47:50+00:00 | `ab995e9e894f` | PASS | 57.14% | 100.00% | 100.00% |
| 2026-09-20T13:53:45+00:00 | `4ad8c24be061` | PASS | 57.16% | 100.00% | 100.00% |

Last fully healthy baseline: `4ad8c24be0615b27f942707fd095362d4f6dd23b`
