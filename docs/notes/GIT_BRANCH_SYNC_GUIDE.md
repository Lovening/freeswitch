# 当前 Git 状态与日常分支同步

本文记录 2026-10-13 的仓库状态，并说明怎样在保留本地修改的情况下开发和同步 FreeSWITCH 官方代码。

## 当前仓库状态

- 当前分支为 `master`，跟踪 `upstream/master`。
- `upstream` 指向官方仓库 `git@github.com:signalwire/freeswitch.git`，推送已禁用，只用于获取官方更新。
- `origin` 指向个人 GitHub 仓库 `git@github.com:Lovening/freeswitch.git`；`gitee` 指向 `git@gitee.com:ZJdate/zj-freeswitch.git`。
- 当前 `HEAD` 为 `5e2edc199b`（`local: preserve current local FreeSWITCH overlay`），父提交是官方提交 `12ac48ab9e`。因此截至本文记录时，官方更新已合入，本地分支比 `upstream/master` 多一个本地覆盖提交。
- 旧仓库历史已整理成以官方代码为父提交的本地覆盖提交。这个提交保存当前源码树；其中包含较广的文件权限和格式差异，后续开发应通过小而清楚的功能提交逐步维护，不要再用整仓覆盖的方式同步。
- 编写本文档前，主仓库没有未提交的源码修改。`src/mod/applications/mod_rtpforward/libwebsockets` 子模块仍有本地状态：示例目录中的 `mount-origin/symlink.html` 是普通 HTML 文件，而子模块记录的是指向 `index.html` 的符号链接；子模块里另有未跟踪的 Android `.gradle/` 构建目录。先保留这些状态；若要保存子模块功能修改，应在子模块中提交，再在主仓库提交更新后的子模块指针。构建缓存不要作为功能提交。
- 本地 `.git/info/exclude` 忽略 `.codex`、`.understand-anything`、编辑器说明、旧备份和本机生成文件。这些规则只对当前克隆生效，不会提交到仓库。
- 上游推送地址设置为 `DISABLED`。`origin` 和 `gitee` 虽可推送，但远程分支可能保留整理前的历史；推送前应检查远程差异，不要对它们强制推送。

查看最新状态：

```bash
git status --short --branch
git branch -vv
git remote -v
git log --oneline --decorate -6
git submodule status
git -C src/mod/applications/mod_rtpforward/libwebsockets status --short
```

## 推荐分支用法

长期只保留 `master` 作为本地集成分支。每个具体功能从 `master` 创建一个临时分支，完成后合并回 `master` 并删除临时分支：

```text
upstream/master  ──官方更新──> master
                                  ├── feature/conference-x
                                  └── feature/mod-av-y
```

功能分支只装一个可以说明和验证的改动。避免把格式化、生成文件、个人工具和多个无关功能放进同一个提交。

## 开始一个功能

先确认当前工作区没有未保存修改，并同步官方更新：

```bash
git switch master
git status --short
git fetch upstream --prune
git log --oneline HEAD..upstream/master
git merge upstream/master
```

如果本地已有未提交修改，先按功能提交或暂存，不要直接切分支覆盖它们。更新后创建功能分支：

```bash
git switch -c feature/简短功能名 master
```

开发过程中检查并提交相关文件：

```bash
git status --short
git diff
git add <本次功能涉及的文件>
git diff --cached
git commit -m "feat: 简述功能"
```

例如：

```bash
git switch -c feature/conference-layout master
# 修改并验证
git add src/mod/applications/mod_conference/conference_video.c
git commit -m "fix(conference): correct layout selection"
```

## 合并功能分支

功能完成并验证后，将功能分支合回 `master`：

```bash
git switch master
git fetch upstream --prune
git merge upstream/master
git merge --no-ff feature/简短功能名
```

合并完成后确认状态和提交图：

```bash
git status --short --branch
git log --graph --oneline --decorate -12
```

功能分支已确认合入后，可以删除本地临时分支：

```bash
git branch -d feature/简短功能名
```

如果 Git 拒绝删除，先检查该分支是否确实已合并；不要用 `-D` 绕过检查。

## 同步未来的官方更新

日常更新时，先提交或暂存正在做的本地改动，再运行：

```bash
git switch master
git status --short
git fetch upstream --prune
git log --oneline HEAD..upstream/master
git merge upstream/master
```

没有冲突时，Git 会创建合并提交，保留本地提交和上游提交。若输出 `Already up to date`，表示当前已包含远端更新。

## 发生冲突时

查看冲突文件：

```bash
git diff --name-only --diff-filter=U
```

逐个检查文件中的 `<<<<<<<`、`=======`、`>>>>>>>` 标记，结合两边功能意图编辑成最终代码。不能不看内容就统一选择本地或上游。解决后：

```bash
git add <已解决的文件>
git status
git merge --continue
```

运行相关构建或测试，再检查最终状态。若决定放弃本次合并：

```bash
git merge --abort
```

这会撤销当前未完成的合并，回到合并前的提交；合并前已提交的本地修改仍保留。

## 子模块修改

子模块是独立 Git 仓库。主仓库的 `git add` 不会提交子模块内部文件。若确实要保存子模块修改：

```bash
git -C src/mod/applications/mod_rtpforward/libwebsockets status
git -C src/mod/applications/mod_rtpforward/libwebsockets add <子模块内文件>
git -C src/mod/applications/mod_rtpforward/libwebsockets commit -m "说明子模块修改"
git add src/mod/applications/mod_rtpforward/libwebsockets
git commit -m "chore: update libwebsockets submodule"
```

若只是工作系统把符号链接展开成普通文件，应先确认其原因，再决定是否恢复符号链接；不要把这种变化误当成 FreeSWITCH 功能提交。

## 推送到个人远程

官方远程只用于拉取，不能推送。向个人远程推送前，先查看远程分支是否与当前历史兼容：

```bash
git fetch origin --prune
git log --oneline --graph --decorate --left-right origin/master...master
```

本地历史曾经重新整理，而个人远程可能仍有旧历史。如果两边历史分叉，不要对 `origin/master` 或 Gitee 的 `master` 执行强制推送。可以先把当前工作推到新的个人分支供备份或评审：

```bash
git push -u origin master:codex/local-integration
```

个人远程的分支名和推送方式确认后再长期使用。推送不会替代本地提交，也不会更新官方仓库。


git fetch upstream
git switch master
git merge --ff-only upstream/master
git switch local
git rebase master
