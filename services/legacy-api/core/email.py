"""Email verification code service.

Uses stdlib smtplib (async via run_in_executor) to send verification codes.
Codes are stored in-memory with TTL; no external dependencies required.
"""
from __future__ import annotations

import logging
import os
import random
import smtplib
import string
import time
from email.mime.text import MIMEText
from functools import lru_cache
from typing import Optional

logger = logging.getLogger(__name__)

_CODE_LENGTH = 6
_CODE_TTL_SECONDS = 300  # 5 min
_CODE_COOLDOWN_SECONDS = 60

_pending_codes: dict[str, tuple[str, float]] = {}


def _generate_code() -> str:
    return "".join(random.choices(string.digits, k=_CODE_LENGTH))


def smtp_configured() -> bool:
    """True 表示 SMTP_HOST / SMTP_USER 都填了，真的会去连邮件服务器。

    没有配置时 `send_verification_code` 只把验证码打进日志并返回成功，
    调用方（登录页找回密码、绑定邮箱）因此看起来像「已发送」。
    接口层用这个函数把 dev 模式显式告诉前端，别再静默成功。
    """
    return bool(os.environ.get("SMTP_HOST") and os.environ.get("SMTP_USER"))


def _smtp_configured() -> bool:  # 向后兼容旧调用
    return smtp_configured()


@lru_cache(maxsize=1)
def _smtp_config() -> dict:
    return {
        "host": os.environ.get("SMTP_HOST", ""),
        "port": int(os.environ.get("SMTP_PORT", "465")),
        "user": os.environ.get("SMTP_USER", ""),
        "password": os.environ.get("SMTP_PASSWORD", ""),
        "from_addr": os.environ.get("SMTP_FROM", os.environ.get("SMTP_USER", "")),
        "use_ssl": os.environ.get("SMTP_SSL", "1") != "0",
    }


def _send_smtp(to_addr: str, subject: str, body: str) -> None:
    cfg = _smtp_config()
    msg = MIMEText(body, "plain", "utf-8")
    msg["Subject"] = subject
    msg["From"] = cfg["from_addr"]
    msg["To"] = to_addr

    if cfg["use_ssl"]:
        with smtplib.SMTP_SSL(cfg["host"], cfg["port"], timeout=10) as srv:
            srv.login(cfg["user"], cfg["password"])
            srv.send_message(msg)
    else:
        with smtplib.SMTP(cfg["host"], cfg["port"], timeout=10) as srv:
            srv.starttls()
            srv.login(cfg["user"], cfg["password"])
            srv.send_message(msg)


async def send_verification_code(email: str) -> tuple[bool, str]:
    """Generate, store, and send a verification code to *email*.

    Returns (success, message).  Rate-limited to one code per email per cooldown window.
    """
    import asyncio

    email = email.strip().lower()

    existing = _pending_codes.get(email)
    if existing and time.time() - existing[1] < _CODE_COOLDOWN_SECONDS:
        remaining = int(_CODE_COOLDOWN_SECONDS - (time.time() - existing[1]))
        return False, f"请 {remaining} 秒后再试"

    code = _generate_code()
    _pending_codes[email] = (code, time.time())

    if not smtp_configured():
        logger.warning(
            "[EMAIL][DEV] SMTP not configured (SMTP_HOST/SMTP_USER empty) — "
            "code for %s is %s, printed to log only, NO mail was sent",
            email,
            code,
        )
        return True, "验证码已发送（开发模式：检查服务器日志）"

    subject = "XiaoWuAI 验证码"
    body = f"您的验证码是：{code}\n\n{_CODE_TTL_SECONDS // 60} 分钟内有效，请勿泄露给他人。"

    try:
        loop = asyncio.get_running_loop()
        await loop.run_in_executor(None, _send_smtp, email, subject, body)
        return True, "验证码已发送"
    except Exception as exc:
        logger.error("[EMAIL] Failed to send to %s: %s", email, exc, exc_info=True)
        return False, "验证码发送失败，请稍后重试"


def verify_code(email: str, code: str) -> bool:
    """Check whether *code* is valid for *email*.  Consumes the code on success."""
    email = email.strip().lower()
    entry = _pending_codes.get(email)
    if not entry:
        return False
    stored_code, ts = entry
    if time.time() - ts > _CODE_TTL_SECONDS:
        _pending_codes.pop(email, None)
        return False
    if stored_code != code.strip():
        return False
    _pending_codes.pop(email, None)
    return True


def purge_expired() -> None:
    """Remove expired codes (call periodically if desired)."""
    now = time.time()
    expired = [k for k, (_, ts) in _pending_codes.items() if now - ts > _CODE_TTL_SECONDS]
    for k in expired:
        _pending_codes.pop(k, None)
