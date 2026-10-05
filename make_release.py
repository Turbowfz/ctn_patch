#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""在 Gitee 与 GitHub 上建 v1.1 Release，并把 ctn_patch.zip 作为附件传上去。

update.json 里的 zipUrl 是
    https://gitee.com/turbowfz/ctn_patch/releases/download/v1.1/ctn_patch.zip
所以必须先有 tag v1.1、再把 zip 传成「附件」，否则管理器下载会 404。

凭据来源：
  - GitHub：从 git credential（凭据管理器里已缓存的 Turbowfz 凭据）读，不落盘
  - Gitee ：环境变量 GITEE_TOKEN

用法：
    GITEE_TOKEN=xxx python make_release.py
"""
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ZIP = HERE / "ctn_patch.zip"
TAG = "v1.1"
NAME = "v1.1"
CHANGELOG = HERE / "CHANGELOG.md"


def body_from_changelog() -> str:
    """取 CHANGELOG 里 v1.1 那一节当 Release 说明。"""
    txt = CHANGELOG.read_text(encoding="utf-8")
    lines = txt.splitlines()
    out, grab = [], False
    for ln in lines:
        if ln.startswith("## "):
            if grab:
                break
            grab = TAG in ln
            continue
        if grab:
            out.append(ln)
    return "\n".join(out).strip() or f"{NAME} 发布"


def github_token() -> str:
    p = subprocess.run(
        ["git", "credential", "fill"],
        input="protocol=https\nhost=github.com\n\n",
        capture_output=True, text=True, check=True)
    for ln in p.stdout.splitlines():
        if ln.startswith("password="):
            return ln.split("=", 1)[1]
    sys.exit("!! 拿不到 GitHub 凭据")


def req(url, data=None, headers=None, method=None):
    r = urllib.request.Request(url, data=data, headers=headers or {}, method=method)
    try:
        with urllib.request.urlopen(r, timeout=120) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def multipart(field: str, filename: str, payload: bytes):
    b = b"--BOUNDARY1234\r\n"
    b += ('Content-Disposition: form-data; name="%s"; filename="%s"\r\n'
          % (field, filename)).encode()
    b += b"Content-Type: application/octet-stream\r\n\r\n"
    b += payload + b"\r\n--BOUNDARY1234--\r\n"
    return b, "multipart/form-data; boundary=BOUNDARY1234"


def gh_release():
    tok = github_token()
    api = "https://api.github.com/repos/Turbowfz/ctn_patch"
    h = {"Authorization": "Bearer " + tok, "Accept": "application/vnd.github+json",
         "User-Agent": "ctn_patch-release"}

    st, raw = req(api + "/releases/tags/" + TAG, headers=h)
    if st == 200:
        rel = json.loads(raw)
        print("  GitHub: Release %s 已存在 (id=%s)" % (TAG, rel["id"]))
    else:
        payload = json.dumps({"tag_name": TAG, "name": NAME,
                              "body": body_from_changelog()}).encode()
        st, raw = req(api + "/releases", data=payload,
                      headers=dict(h, **{"Content-Type": "application/json"}))
        if st not in (200, 201):
            sys.exit("!! GitHub 建 Release 失败 %s: %s" % (st, raw[:400]))
        rel = json.loads(raw)
        print("  GitHub: 已建 Release %s (id=%s)" % (TAG, rel["id"]))

    names = {a["name"] for a in rel.get("assets", [])}
    if "ctn_patch.zip" in names:
        print("  GitHub: 附件已存在，跳过上传")
        return
    data, ctype = multipart("file", "ctn_patch.zip", ZIP.read_bytes())
    st, raw = req(rel["upload_url"].split("{")[0] + "?name=ctn_patch.zip",
                  data=data, headers=dict(h, **{"Content-Type": ctype}))
    if st not in (200, 201):
        sys.exit("!! GitHub 传附件失败 %s: %s" % (st, raw[:400]))
    print("  GitHub: 附件已上传 ->", json.loads(raw)["browser_download_url"])


def gitee_release(tok: str):
    api = "https://gitee.com/api/v5/repos/turbowfz/ctn_patch/releases"
    st, raw = req(api + "/tags/" + TAG + "?access_token=" + tok)
    # 注意：Gitee 在「这个 tag 还没有 Release」时也返回 200，body 是字面量 null，
    # 不是 404 —— 只判状态码会拿到 None 然后在下面炸掉。
    rel = json.loads(raw) if (st == 200 and raw.strip() not in (b"null", b"")) else None
    if rel:
        print("  Gitee : Release %s 已存在 (id=%s)" % (TAG, rel["id"]))
    else:
        body = urllib.parse.urlencode({
            "access_token": tok, "tag_name": TAG, "name": NAME,
            "body": body_from_changelog(), "target_commitish": "main",
        }).encode()
        st, raw = req(api, data=body,
                      headers={"Content-Type": "application/x-www-form-urlencoded"})
        if st not in (200, 201):
            sys.exit("!! Gitee 建 Release 失败 %s: %s" % (st, raw[:400]))
        rel = json.loads(raw)
        if not rel:
            sys.exit("!! Gitee 建 Release 返回空: %s" % raw[:400])
        print("  Gitee : 已建 Release %s (id=%s)" % (TAG, rel["id"]))

    if rel.get("attach_files"):
        print("  Gitee : 附件已存在，跳过上传")
        return
    data, ctype = multipart("file", "ctn_patch.zip", ZIP.read_bytes())
    st, raw = req("%s/%s/attach_files?access_token=%s" % (api, rel["id"], tok),
                  data=data, headers={"Content-Type": ctype})
    if st not in (200, 201):
        sys.exit("!! Gitee 传附件失败 %s: %s" % (st, raw[:400]))
    d = json.loads(raw)
    print("  Gitee : 附件已上传 ->", d.get("browser_download_url") or d)


def main():
    import urllib.parse  # noqa: F401  （gitee_release 里用到）
    if not ZIP.is_file():
        sys.exit("!! 找不到 %s，先跑 python build_zip.py" % ZIP)
    tok = os.environ.get("GITEE_TOKEN")
    if not tok:
        sys.exit("!! 需要 GITEE_TOKEN 环境变量")

    print("=== GitHub ===")
    gh_release()
    print("=== Gitee ===")
    gitee_release(tok)
    print("\n完成。update.json 的 zipUrl 指向：")
    print("  https://gitee.com/turbowfz/ctn_patch/releases/download/%s/ctn_patch.zip" % TAG)


if __name__ == "__main__":
    main()
