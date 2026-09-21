# SwordFS POSIX conformance status

Latest run: **PASS**

| Metric | Value |
| --- | ---: |
| Overall conformance support | 58.91% |
| Supported regression pass | 100.00% |
| Classified rate | 100.00% |
| Supported assertion count | 5166 |
| Blocking result count | 0 |

## Classification counts

| Classification | Count |
| --- | ---: |
| KNOWN_SEMANTIC_DEFECT | 51 |
| KNOWN_UNSUPPORTED | 3553 |
| PASS | 5166 |
| UPSTREAM_NOT_APPLICABLE | 28 |

## Per-category support

| Category | PASS | Applicable | Support |
| --- | ---: | ---: | ---: |
| chflags | 14 | 14 | 100.00% |
| chmod | 198 | 327 | 60.55% |
| chown | 648 | 1470 | 44.08% |
| ftruncate | 88 | 89 | 98.88% |
| granular | 7 | 7 | 100.00% |
| link | 198 | 359 | 55.15% |
| mkdir | 95 | 118 | 80.51% |
| mkfifo | 50 | 120 | 41.67% |
| mknod | 62 | 186 | 33.33% |
| open | 258 | 337 | 76.56% |
| posix_fallocate | 1 | 1 | 100.00% |
| rename | 2904 | 4857 | 59.79% |
| rmdir | 125 | 145 | 86.21% |
| symlink | 87 | 95 | 91.58% |
| truncate | 83 | 84 | 98.81% |
| unlink | 246 | 439 | 56.04% |
| utimensat | 102 | 122 | 83.61% |

## Known-gap Issues

| Issue | Classification | Assertions | Reason |
| --- | --- | ---: | --- |
| [#208](https://github.com/SwordInfra/SwordFS/issues/208) | KNOWN_UNSUPPORTED | 344 | dependent assertion after unsupported special-file/FIFO/socket setup |
| [#208](https://github.com/SwordInfra/SwordFS/issues/208) | KNOWN_UNSUPPORTED | 3209 | special-file/FIFO/socket node semantics are not implemented |
| [#211](https://github.com/SwordInfra/SwordFS/issues/211) | KNOWN_SEMANTIC_DEFECT | 9 | overlong path components must return ENAMETOOLONG consistently |
| [#213](https://github.com/SwordInfra/SwordFS/issues/213) | KNOWN_SEMANTIC_DEFECT | 42 | pathname resolution or namespace error/transition semantics differ from POSIX |

## Known-gap assertions

| Assertion | Classification | Issue | Reason |
| --- | --- | --- | --- |
| `tests/chmod/00.t#22` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#23` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#24` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#26` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chmod/00.t#27` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chmod/00.t#28` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#31` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#32` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#33` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#34` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#36` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chmod/00.t#37` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chmod/00.t#38` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#41` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#42` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#43` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#44` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#46` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chmod/00.t#47` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chmod/00.t#48` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#51` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#52` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#53` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#54` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#56` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chmod/00.t#57` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chmod/00.t#58` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#61` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#70` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#71` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#72` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chmod/00.t#73` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#74` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#75` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#76` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chmod/00.t#77` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#78` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#79` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#80` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chmod/00.t#81` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#82` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#83` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#84` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chmod/00.t#85` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#94` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#95` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#96` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chmod/00.t#97` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#98` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#99` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#100` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chmod/00.t#101` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#102` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#103` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#104` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chmod/00.t#105` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#106` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#107` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/00.t#108` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chmod/00.t#109` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
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
| `tests/chmod/11.t#18` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#19` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#20` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#22` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chmod/11.t#23` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#25` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#26` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#27` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#28` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#30` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chmod/11.t#31` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#33` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#34` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#35` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#36` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#38` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chmod/11.t#39` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#41` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#42` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#43` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#44` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#46` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chmod/11.t#47` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#49` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#69` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#70` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#71` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#73` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#74` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#75` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chmod/11.t#76` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#78` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#79` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#80` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#81` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#83` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#84` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#85` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chmod/11.t#86` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#88` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#89` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#90` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#91` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#93` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#94` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#95` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chmod/11.t#96` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#98` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#99` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#100` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#101` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#103` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#104` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#105` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chmod/11.t#106` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chmod/11.t#108` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#34` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#35` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#36` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#37` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#38` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#40` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chown/00.t#41` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
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
| `tests/chown/00.t#56` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chown/00.t#57` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
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
| `tests/chown/00.t#72` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chown/00.t#73` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
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
| `tests/chown/00.t#88` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chown/00.t#89` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
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
| `tests/chown/00.t#167` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#168` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#170` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chown/00.t#171` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#172` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chown/00.t#174` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chown/00.t#175` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#176` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chown/00.t#178` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chown/00.t#179` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#180` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | dependent assertion after unsupported special-file/FIFO/socket setup |
| `tests/chown/00.t#183` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#184` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |
| `tests/chown/00.t#185` | KNOWN_UNSUPPORTED | [#208](https://github.com/SwordInfra/SwordFS/issues/208) | special-file/FIFO/socket node semantics are not implemented |

_Only the first 200 of 3604 known gaps are shown; see `result.json`._

## Baseline metadata

- SwordFS commit: `5f54cff705f3cc791f800a93fc417100f46b95cb`
- pjdfstest commit: `85a8aea9e685999ef0540392fd80535f873d7ff7`
- kernel: `6.17.0-1022-azure`
- libfuse: `fusermount3 version: 3.18.2`
- os: `Linux-6.17.0-1022-azure-x86_64-with-glibc2.43`
- runner: `Linux`

Authoritative CI run: https://github.com/SwordInfra/SwordFS/actions/runs/35553605837
Recorded at: 2026-09-21T02:23:39+00:00

## Historical main-branch trend

| Time | SwordFS | Status | Overall support | Supported gate | Classified |
| --- | --- | --- | ---: | ---: | ---: |
| 2026-09-20T08:18:31+00:00 | `e844e9a8df64` | PASS | 57.14% | 100.00% | 100.00% |
| 2026-09-20T13:53:45+00:00 | `4ad8c24be061` | PASS | 57.16% | 100.00% | 100.00% |
| 2026-09-21T01:48:15+00:00 | `8deedcedadd0` | PASS | 58.91% | 100.00% | 100.00% |

Last fully healthy baseline: `5f54cff705f3cc791f800a93fc417100f46b95cb`
