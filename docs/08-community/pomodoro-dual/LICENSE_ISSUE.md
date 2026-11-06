Hi — thanks for publishing this. The preset/cycle model and the partial-window
refresh handling are both better than what I had put together, and the e-paper
timer space is thin enough that this probably gets more use than you'd guess.

This isn't about my PR (#3) — I wanted to ask something separate, because it
affects everyone who lands here:

**The repo has no `LICENSE` file.** GitHub reports the licence as `null`, which
means the default is *all rights reserved*, not public domain. Reading and
forking are fine, but nobody can legally *use*, modify, or ship a build of this
without your explicit permission.

That seems at odds with the intent of putting it up. It's at ~1.1k stars and
39 forks with zero open issues, which usually means people are quietly building
on it without a clear legal basis — they just can't say so, or ship it.

Would you consider adding one?

- **MIT** is the usual fit for a personal hardware project: short, permissive,
  keeps your copyright intact.
- **Apache-2.0** adds an explicit patent grant, which some companies need before
  they'll touch a repo.
- **GPL-3.0** if you'd rather derivatives stay open.

It's one file plus a line in the README, and I'm happy to open that PR if you'd
rather not pick one yourself.

For full disclosure of where I'm coming from: I maintain a dual-screen e-paper
platform — https://dual.xiaowuai.vip/ — which is the device behind #3. It renders
pages server-side and doesn't run this firmware, and I did not port any code
from here into it. I asked about the licence because if one lands, the honest
thing is to point people at your repo for the Arduino build rather than keep a
private copy.

Thanks for reading either way.
