---
description: 根据 #changes 生成 Conventional Commits 提交信息
mode: ask
---

基于 `#changes` 生成：

- 标题 `<type>(<scope>): <desc>` ≤ 60 字
  - scope ∈ app / native / inject / stealth / ue / build
- 正文：What / Why
- BREAKING CHANGE 当 JNI 签名或注入器接口变更

输出纯文本。
