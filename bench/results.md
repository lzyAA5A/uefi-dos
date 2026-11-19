# UEFI-DOS benchmark results

Generated 2026-11-20 01:53; TSC 2,419,200,094 Hz; host harness runs: 3; UEFI accelerator: whpx

| workload | unit | 16-bit DOS VM | native 64-bit | native 32-bit | 64-bit vs DOS | 32-bit vs DOS |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| alu | ns/step | 1,424.8 | 4.0 | 4.7 | 358.4 | 178.5 |
| fill | MB/s | 7.6 | 5,153.5 | 8,529.4 | 674.9 | 744.6 |
| copy | MB/s | 3.9 | 4,653.6 | 4,536.5 | 1,191.7 | 580.9 |
| scan | MB/s | 3.4 | 3,743.3 | 5,088.9 | 1,111.1 | 1,066.2 |
| api | ns/call | 2,095.4 | 1.1 | 0.8 | 1,836.8 | 198.6 |
| text | ns/char | 209.5 | 21.2 | 24,674.5 | 9.9 | 1.0 |
| file | MB/s | - | 110.3 | 0.0 | - | 1.2 |
| find | ns/entry | - | 4,803.3 | 55,233,950.1 | - | 1.0 |

Time per workload (microseconds, median of the runs):

| workload | 16-bit DOS VM (host) | native 64-bit (host) | 16-bit DOS VM (UEFI) | native 32-bit (UEFI) |
| --- | ---: | ---: | ---: | ---: |
| alu | 933,732 | 2,606 | 549,254 | 3,078 |
| fill | 823,881 | 1,221 | 549,254 | 738 |
| copy | 604,179 | 507 | 302,090 | 520 |
| scan | 933,732 | 840 | 659,105 | 618 |
| api | 1,098,508 | 598 | 82,388 | 415 |
| text | 659,105 | 66,779 | 77,774,366 | 77,614,523 |
| file | 0 | 2,396 | 29,220,313 | 25,299,725 |
| find | 0 | 5,764 | 67,860,332 | 66,280,740 |
