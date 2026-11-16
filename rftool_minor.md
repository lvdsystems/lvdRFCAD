# RF tool: minor changes

Small improvements found while using the RF tools.

## Implemented (2026-11-16, before release 0.1.0)

### 1. Mixer: explicit wanted product

`rf-system:mixer` has a parameter `product` (shown on the symbol): `auto` (default), `difference` or `sum`.

- `sum` and `difference` select f_in + f_LO or |f_in - f_LO| whichever pin the signal enters.
- `auto` keeps the previous rule, so existing designs give the same results: the difference when the signal enters at `rf`, and by `side` when it enters at `if` (`low` gives the sum).
- The budget table shows the product next to the mixer's frequencies ("1 GHz → 16 GHz (sum)"), and the Python frequency plan has a `product` entry.

Code: `SignalChain.cpp` (frequency plan), `Budget.cpp` (`mixerProductName`), `BudgetModels.cpp`, library, validator and docs. Tests: `budget_tests.cpp`, "the mixer's product parameter selects sum or difference…".

### 2. Budget: warning for a likely wrong product

For every mixer, the stages up to the next frequency translation are evaluated at both products. If the other product passes them at least 20 dB better than the selected one, the budget warns, for example:

> MX1: the following stages pass 16 GHz (the sum product) but attenuate the selected difference product at 14 GHz by 33 dB. Set the mixer's "product" to "sum"?

Code: `checkMixerProducts` in `Budget.cpp`, called by `runBudget`. Tests: same test case.

## Open

Nothing at the moment.
