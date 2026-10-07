#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""通过 GitHub Git Data API 推一个本地提交（github.com:443 连不上时的备用通道）。

只适用于「在已有父提交之上追加一个提交」这种情况：把本地那个提交的
tree / parent / author / committer / date / message 原样搬过去，
算出来的 SHA 和本地一致 —— 这样两边历史不会分叉。

用法：
    python gh_api_push.py <本地提交sha> <远端父提交sha> [分支]
例：
    python gh_api_push.py ff1cffc 17c448a main
"""
import base64
import datetime
import json
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

REPO = "Turbowfz/ctn_patch"
API = "https://api.github.com/repos/" + REPO


def gh_token() -> str:
    p = subprocess.run(["git", "credential", "fill"],
                       input="protocol=https\nhost=github.com\n\n",
                       capture_output=True, text=True, check=True)
    for ln in p.stdout.splitlines():
        if ln.startswith("password="):
            return ln.split("=", 1)[1]
    sys.exit("!! 拿不到 GitHub 凭据")


TOKEN = None


def req(path, data=None, method=None):
    url = API + path
    h = {"Authorization": "Bearer " + TOKEN,
         "Accept": "application/vnd.github+json",
         "User-Agent": "ctn_patch-push"}
    if data is not None:
        data = json.dumps(data).encode()
        h["Content-Type"] = "application/json"
    r = urllib.request.Request(url, data=data, headers=h, method=method)
    try:
        with urllib.request.urlopen(r, timeout=120) as resp:
            return resp.status, json.loads(resp.read() or b"null")
    except urllib.error.HTTPError as e:
        body = e.read()
        sys.exit("!! %s %s -> %s: %s" % (method or "GET", path, e.code, body[:500]))


def git(*args, binary=False):
    p = subprocess.run(["git"] + list(args), capture_output=True,
                       check=True, cwd=Path(__file__).resolve().parent)
    return p.stdout if binary else p.stdout.decode("utf-8")


def main():
    global TOKEN
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    local, parent = sys.argv[1], sys.argv[2]
    branch = sys.argv[3] if len(sys.argv) > 3 else "main"
    TOKEN = gh_token()

    # GitHub 的 API 只收完整 40 位 sha，短 sha 一律 422
    local = git("rev-parse", local).strip()
    parent = git("rev-parse", parent).strip()

    meta = git("cat-file", "-p", local)
    lines = meta.splitlines()
    tree = next(l.split()[1] for l in lines if l.startswith("tree "))
    msg = meta.split("\n\n", 1)[1]
    au = next(l for l in lines if l.startswith("author "))
    co = next(l for l in lines if l.startswith("committer "))

    def who(line):
        # "author Turbo <turboemail@qq.com> 1791209175 +0800"
        # GitHub 只认 ISO 8601，不认 git 的 "<epoch> <tz>"，得转一下。
        rest = line.split(" ", 1)[1]
        name, rest = rest.split(" <", 1)
        email, rest = rest.split("> ", 1)
        ts, tz = rest.split(" ")
        sign = 1 if tz[0] == "+" else -1
        off = datetime.timezone(sign * datetime.timedelta(
            hours=int(tz[1:3]), minutes=int(tz[3:5])))
        return {"name": name, "email": email,
                "date": datetime.datetime.fromtimestamp(int(ts), off).isoformat()}

    changed = git("diff", "--name-status", parent, local).strip().splitlines()
    base_tree = git("rev-parse", parent + "^{tree}").strip()
    print("本地提交 %s -> 父 %s（远端 tree %s）" % (local[:7], parent[:7], base_tree[:7]))

    entries = []
    for ln in changed:
        st, path = ln.split("	", 1)
        if st == "D":
            # 删除文件：tree 条目 sha 设 null，GitHub 会把它从树里去掉。
            # （原来没处理这分支 —— `git show <提交>:<被删路径>` 直接失败，
            #   整个推送崩在那里，实测踩过。）
            entries.append({"path": path, "mode": "100644",
                            "type": "blob", "sha": None})
            print("  del  %-20s (删除)" % path)
            continue
        blob = git("show", "%s:%s" % (local, path), binary=True)
        _, res = req("/git/blobs", {"content": base64.b64encode(blob).decode(),
                                    "encoding": "base64"}, "POST")
        mode = git("ls-tree", local, path).split()[0]
        entries.append({"path": path, "mode": mode, "type": "blob",
                        "sha": res["sha"]})
        print("  blob %-20s %s (%s)" % (path, res["sha"][:8], st))

    _, nt = req("/git/trees", {"base_tree": base_tree, "tree": entries}, "POST")
    print("  新 tree", nt["sha"][:8])

    _, cm = req("/git/commits", {"message": msg, "tree": nt["sha"],
                                 "parents": [parent],
                                 "author": who(au), "committer": who(co)}, "POST")
    print("  新 commit", cm["sha"][:8])

    if cm["sha"] != git("rev-parse", local).strip():
        sys.exit("!! 生成的 SHA 与本地不一致（%s != %s），停手不推"
                 % (cm["sha"], local))

    _, ref = req("/git/refs/heads/" + branch, method="GET")
    print("  远端 %s 现在指向 %s" % (branch, ref["object"]["sha"][:8]))
    _, upd = req("/git/refs/heads/" + branch,
                 {"sha": cm["sha"], "force": True}, "PATCH")
    print("  已更新 %s -> %s" % (branch, upd["object"]["sha"][:8]))
    print("\n完成：两边历史一致（SHA 相同，没有分叉）")


if __name__ == "__main__":
    main()
