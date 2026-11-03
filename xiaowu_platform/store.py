import hashlib
import json
import secrets
import sqlite3
from contextlib import contextmanager
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any, Iterator


def now_iso() -> str:
    return datetime.now(UTC).isoformat()


def _mac_candidates(device_id: str | None) -> list[str]:
    """Return candidate device_id keys for a MAC, covering the two spellings
    seen across the fleet: colon-separated (DC:DA:0C:44:6D:D4) and the bare
    12-hex form (DCDA0C446DD4).

    The firmware always presents ``dc:da:0c:44:6d:d4`` (lowercase, colon),
    but tokens/configs were historically stored under either spelling. Trying
    both on lookup lets a token or config resolve no matter which row exists,
    which is what keeps ``dual_mode_`` detectable on the device.
    """
    if not device_id:
        return []
    s = device_id.strip().upper()
    digits = "".join(ch for ch in s if ch in "0123456789ABCDEF")
    if len(digits) != 12:
        return [s]
    colon = ":".join(digits[i : i + 2] for i in range(0, 12, 2))
    cands: list[str] = []
    for c in (s, colon, digits):
        if c and c not in cands:
            cands.append(c)
    return cands


class Store:
    def __init__(self, path: Path):
        self.path = path

    @contextmanager
    def connect(self) -> Iterator[sqlite3.Connection]:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        conn = sqlite3.connect(self.path)
        conn.row_factory = sqlite3.Row
        try:
            yield conn
            conn.commit()
        finally:
            conn.close()

    def init(self) -> None:
        with self.connect() as db:
            db.executescript("""
            CREATE TABLE IF NOT EXISTS devices (
              device_id TEXT PRIMARY KEY, model TEXT NOT NULL, firmware_version TEXT NOT NULL,
              token_hash TEXT NOT NULL, capabilities TEXT NOT NULL, created_at TEXT NOT NULL,
              last_seen_at TEXT, battery_voltage REAL, rssi INTEGER, runtime_mode TEXT
            );
            CREATE TABLE IF NOT EXISTS tasks (
              id TEXT PRIMARY KEY, device_id TEXT NOT NULL, payload TEXT NOT NULL,
              status TEXT NOT NULL, created_at TEXT NOT NULL, expires_at TEXT NOT NULL,
              acknowledged_at TEXT, detail TEXT, source TEXT NOT NULL DEFAULT 'openapi',
              created_by TEXT,
              FOREIGN KEY(device_id) REFERENCES devices(device_id)
            );
            CREATE INDEX IF NOT EXISTS idx_tasks_device_status ON tasks(device_id, status, created_at);
            CREATE TABLE IF NOT EXISTS device_bindings (
              user_id TEXT NOT NULL, device_id TEXT NOT NULL, nickname TEXT NOT NULL,
              PRIMARY KEY(user_id, device_id), FOREIGN KEY(device_id) REFERENCES devices(device_id)
            );
            CREATE TABLE IF NOT EXISTS templates (
              template_id TEXT PRIMARY KEY, name TEXT NOT NULL, category TEXT NOT NULL,
              schema_definition TEXT NOT NULL, enabled INTEGER NOT NULL, updated_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS audit_logs (
              id INTEGER PRIMARY KEY AUTOINCREMENT, actor TEXT NOT NULL, action TEXT NOT NULL,
              target TEXT NOT NULL, detail TEXT NOT NULL, created_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS users (
              user_id TEXT PRIMARY KEY, username TEXT UNIQUE NOT NULL,
              password_hash TEXT NOT NULL, password_salt TEXT NOT NULL, created_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS user_sessions (
              token_hash TEXT PRIMARY KEY, user_id TEXT NOT NULL, expires_at TEXT NOT NULL,
              FOREIGN KEY(user_id) REFERENCES users(user_id)
            );
            CREATE TABLE IF NOT EXISTS content_configs (
              device_id TEXT PRIMARY KEY, payload TEXT NOT NULL, updated_at TEXT NOT NULL,
              FOREIGN KEY(device_id) REFERENCES devices(device_id)
            );
            CREATE TABLE IF NOT EXISTS device_claims (
              code TEXT PRIMARY KEY, device_id TEXT NOT NULL, created_at TEXT NOT NULL,
              FOREIGN KEY(device_id) REFERENCES devices(device_id)
            );
            CREATE TABLE IF NOT EXISTS teacher_auth (
              device_id TEXT PRIMARY KEY, frozen INTEGER NOT NULL DEFAULT 0,
              frozen_by TEXT, frozen_at TEXT, FOREIGN KEY(device_id) REFERENCES devices(device_id)
            );
            CREATE TABLE IF NOT EXISTS user_profiles (
              user_id TEXT NOT NULL, category TEXT NOT NULL, data TEXT NOT NULL, updated_at TEXT NOT NULL,
              PRIMARY KEY(user_id, category), FOREIGN KEY(user_id) REFERENCES users(user_id)
            );
            CREATE TABLE IF NOT EXISTS service_configs (
              key TEXT PRIMARY KEY, value TEXT NOT NULL, updated_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS teacher_codes (
              id INTEGER PRIMARY KEY AUTOINCREMENT, device_id TEXT NOT NULL, auth_code TEXT NOT NULL,
              auth_name TEXT NOT NULL, permission TEXT NOT NULL, created_by TEXT NOT NULL,
              created_at TEXT NOT NULL, expire_at TEXT, frozen INTEGER NOT NULL DEFAULT 0,
              FOREIGN KEY(device_id) REFERENCES devices(device_id)
            );
            CREATE TABLE IF NOT EXISTS wechat_openid_map (
              openid TEXT PRIMARY KEY, user_id TEXT NOT NULL, created_at TEXT NOT NULL,
              FOREIGN KEY(user_id) REFERENCES users(user_id)
            );
            CREATE TABLE IF NOT EXISTS push_record (
              id INTEGER PRIMARY KEY AUTOINCREMENT, device_id TEXT NOT NULL, push_type TEXT NOT NULL,
              sender_type TEXT, sender_name TEXT, content TEXT, is_append INTEGER DEFAULT 1,
              item_count INTEGER DEFAULT 0, create_time TEXT NOT NULL,
              FOREIGN KEY(device_id) REFERENCES devices(device_id)
            );
            CREATE TABLE IF NOT EXISTS device_tokens (
              device_id TEXT PRIMARY KEY, token_hash TEXT NOT NULL, created_at TEXT NOT NULL,
              FOREIGN KEY(device_id) REFERENCES devices(device_id)
            );
            CREATE TABLE IF NOT EXISTS developer_apps (
              app_id TEXT PRIMARY KEY, name TEXT NOT NULL, token_hash TEXT UNIQUE NOT NULL,
              scopes TEXT NOT NULL, status TEXT NOT NULL, created_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS developer_accounts (
              developer_id TEXT PRIMARY KEY, name TEXT NOT NULL, organization TEXT NOT NULL,
              role TEXT NOT NULL DEFAULT 'developer',
              email TEXT UNIQUE NOT NULL, password_hash TEXT NOT NULL, password_salt TEXT NOT NULL,
              agreement_version TEXT NOT NULL, agreement_accepted_at TEXT NOT NULL,
              status TEXT NOT NULL DEFAULT 'active', created_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS developer_sessions (
              token_hash TEXT PRIMARY KEY, developer_id TEXT NOT NULL, expires_at TEXT NOT NULL,
              FOREIGN KEY(developer_id) REFERENCES developer_accounts(developer_id)
            );
            CREATE TABLE IF NOT EXISTS admin_accounts (
              admin_id TEXT PRIMARY KEY, username TEXT UNIQUE NOT NULL,
              password_hash TEXT NOT NULL, password_salt TEXT NOT NULL,
              display_name TEXT NOT NULL, status TEXT NOT NULL, created_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS admin_sessions (
              token_hash TEXT PRIMARY KEY, admin_id TEXT NOT NULL, expires_at TEXT NOT NULL,
              FOREIGN KEY(admin_id) REFERENCES admin_accounts(admin_id)
            );
            CREATE TABLE IF NOT EXISTS sandbox_apps (
              app_id TEXT PRIMARY KEY, developer_id TEXT NOT NULL, name TEXT NOT NULL,
              scene TEXT NOT NULL, description TEXT NOT NULL, status TEXT NOT NULL,
              sandbox_token_hash TEXT UNIQUE NOT NULL, version TEXT NOT NULL DEFAULT '0.1.0',
              review_note TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
              FOREIGN KEY(developer_id) REFERENCES developer_accounts(developer_id)
            );
            CREATE TABLE IF NOT EXISTS developer_model_configs (
              app_id TEXT PRIMARY KEY, provider TEXT NOT NULL, model TEXT NOT NULL,
              base_url TEXT NOT NULL, credential_encrypted TEXT NOT NULL,
              system_prompt TEXT NOT NULL, updated_at TEXT NOT NULL,
              FOREIGN KEY(app_id) REFERENCES sandbox_apps(app_id)
            );
            CREATE TABLE IF NOT EXISTS sandbox_builds (
              app_id TEXT PRIMARY KEY, repo_url TEXT NOT NULL, repo_slug TEXT NOT NULL DEFAULT '',
              branch TEXT NOT NULL DEFAULT '', commit_sha TEXT NOT NULL DEFAULT '',
              entry TEXT NOT NULL DEFAULT '', verdict TEXT NOT NULL DEFAULT 'pass',
              report TEXT NOT NULL DEFAULT '{}', file_count INTEGER NOT NULL DEFAULT 0,
              size_bytes INTEGER NOT NULL DEFAULT 0, imported_at TEXT NOT NULL,
              FOREIGN KEY(app_id) REFERENCES sandbox_apps(app_id)
            );
            CREATE TABLE IF NOT EXISTS webhooks (
              id TEXT PRIMARY KEY, app_id TEXT NOT NULL, url TEXT NOT NULL, events TEXT NOT NULL,
              signing_secret TEXT NOT NULL, enabled INTEGER NOT NULL, created_at TEXT NOT NULL,
              FOREIGN KEY(app_id) REFERENCES developer_apps(app_id)
            );
            CREATE TABLE IF NOT EXISTS device_events (
              id TEXT PRIMARY KEY, device_id TEXT NOT NULL, event_type TEXT NOT NULL,
              data TEXT NOT NULL, occurred_at TEXT NOT NULL, received_at TEXT NOT NULL,
              FOREIGN KEY(device_id) REFERENCES devices(device_id)
            );
            CREATE TABLE IF NOT EXISTS webhook_outbox (
              id TEXT PRIMARY KEY, webhook_id TEXT NOT NULL, event_type TEXT NOT NULL,
              payload TEXT NOT NULL, status TEXT NOT NULL, attempts INTEGER NOT NULL,
              created_at TEXT NOT NULL, delivered_at TEXT,
              FOREIGN KEY(webhook_id) REFERENCES webhooks(id)
            );
            CREATE TABLE IF NOT EXISTS user_connectors (
              user_id TEXT NOT NULL, connector_id TEXT NOT NULL,
              credential_encrypted TEXT NOT NULL, external_account TEXT NOT NULL DEFAULT '',
              scopes TEXT NOT NULL DEFAULT '[]', status TEXT NOT NULL DEFAULT 'active',
              expires_at TEXT, updated_at TEXT NOT NULL,
              PRIMARY KEY(user_id, connector_id),
              FOREIGN KEY(user_id) REFERENCES users(user_id)
            );
            CREATE TABLE IF NOT EXISTS connector_auth_states (
              state TEXT PRIMARY KEY, user_id TEXT NOT NULL, connector_id TEXT NOT NULL,
              created_at TEXT NOT NULL, expires_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS messages (
              id TEXT PRIMARY KEY, device_id TEXT NOT NULL, sender_name TEXT NOT NULL,
              sender_type TEXT NOT NULL DEFAULT 'member', sender_id TEXT,
              kind TEXT NOT NULL DEFAULT 'text', text TEXT, audio_id TEXT, audio_mime TEXT,
              duration REAL, played_at TEXT, created_at TEXT NOT NULL, source TEXT NOT NULL DEFAULT 'console',
              FOREIGN KEY(device_id) REFERENCES devices(device_id)
            );
            CREATE INDEX IF NOT EXISTS idx_messages_device_created ON messages(device_id, created_at DESC);
            CREATE TABLE IF NOT EXISTS published_apps (
              app_id TEXT PRIMARY KEY, name TEXT NOT NULL, description TEXT NOT NULL DEFAULT '',
              scene TEXT NOT NULL, preview_url TEXT NOT NULL, entry TEXT NOT NULL DEFAULT '',
              developer_id TEXT NOT NULL DEFAULT '', enabled INTEGER NOT NULL DEFAULT 1,
              review_note TEXT NOT NULL DEFAULT '', published_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS app_data_sources (
              app_id TEXT PRIMARY KEY, endpoint TEXT NOT NULL DEFAULT '',
              token TEXT NOT NULL DEFAULT '', sample_json TEXT NOT NULL DEFAULT '{}',
              verdict TEXT NOT NULL DEFAULT 'fail', checks_json TEXT NOT NULL DEFAULT '[]',
              checked_at TEXT NOT NULL DEFAULT '', last_error TEXT NOT NULL DEFAULT '',
              updated_at TEXT NOT NULL,
              FOREIGN KEY(app_id) REFERENCES sandbox_apps(app_id)
            );
            CREATE TABLE IF NOT EXISTS user_scenes (
              user_id TEXT NOT NULL,
              scene_key TEXT NOT NULL,
              name TEXT NOT NULL,
              eyebrow TEXT NOT NULL DEFAULT '',
              desc TEXT NOT NULL DEFAULT '',
              icon TEXT NOT NULL DEFAULT '',
              created_at TEXT NOT NULL,
              updated_at TEXT NOT NULL,
              PRIMARY KEY(user_id, scene_key)
            );
            CREATE TABLE IF NOT EXISTS pairing_codes (
              code TEXT PRIMARY KEY, app_id TEXT NOT NULL, developer_id TEXT NOT NULL DEFAULT '',
              label TEXT NOT NULL DEFAULT '', scopes TEXT NOT NULL DEFAULT '[]',
              status TEXT NOT NULL DEFAULT 'pending', device_id TEXT, user_id TEXT, grant_id TEXT,
              created_at TEXT NOT NULL, expires_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS app_device_grants (
              grant_id TEXT PRIMARY KEY, app_id TEXT NOT NULL, device_id TEXT NOT NULL,
              user_id TEXT NOT NULL, scopes TEXT NOT NULL DEFAULT '[]', label TEXT NOT NULL DEFAULT '',
              status TEXT NOT NULL DEFAULT 'active', created_at TEXT NOT NULL,
              revoked_at TEXT, last_push_at TEXT,
              UNIQUE(app_id, device_id)
            );
            CREATE INDEX IF NOT EXISTS idx_grants_device ON app_device_grants(device_id, status);
            CREATE INDEX IF NOT EXISTS idx_grants_user ON app_device_grants(user_id);
            """)
            columns = {row[1] for row in db.execute("PRAGMA table_info(devices)")}
            if "pending_refresh" not in columns:
                db.execute("ALTER TABLE devices ADD COLUMN pending_refresh INTEGER NOT NULL DEFAULT 0")
            if "storage_json" not in columns:
                db.execute("ALTER TABLE devices ADD COLUMN storage_json TEXT")
            columns = {row[1] for row in db.execute("PRAGMA table_info(tasks)")}
            if "source" not in columns:
                db.execute("ALTER TABLE tasks ADD COLUMN source TEXT NOT NULL DEFAULT 'openapi'")
            if "created_by" not in columns:
                db.execute("ALTER TABLE tasks ADD COLUMN created_by TEXT")
            developer_columns = {row[1] for row in db.execute("PRAGMA table_info(developer_accounts)")}
            if "role" not in developer_columns:
                db.execute("ALTER TABLE developer_accounts ADD COLUMN role TEXT NOT NULL DEFAULT 'developer'")
            for item in DEFAULT_TEMPLATES:
                db.execute(
                    "INSERT OR IGNORE INTO templates VALUES(?,?,?,?,?,?)",
                    (item["template_id"], item["name"], item["category"],
                     json.dumps(item["schema_definition"], ensure_ascii=False), 1, now_iso()),
                )
            self._ensure_demo_user(db)
            self._ensure_demo_app(db)
            db.execute("UPDATE admin_accounts SET status='disabled' WHERE username='demo'")
            db.execute("DELETE FROM admin_sessions WHERE admin_id IN "
                       "(SELECT admin_id FROM admin_accounts WHERE username='demo')")

    @staticmethod
    def hash_token(token: str) -> str:
        return hashlib.sha256(token.encode()).hexdigest()

    def register(self, device_id: str, model: str, firmware_version: str, capabilities: dict[str, Any]) -> str:
        token = secrets.token_urlsafe(32)
        with self.connect() as db:
            db.execute(
                "INSERT INTO devices VALUES (?, ?, ?, ?, ?, ?, NULL, NULL, NULL, NULL, 0)",
                (device_id, model, firmware_version, self.hash_token(token), json.dumps(capabilities), now_iso()),
            )
            # 生成设备配对码（6 位大写字母数字），供用户在内容后台扫码/输入绑定
            code = secrets.token_hex(3).upper()
            db.execute("INSERT OR REPLACE INTO device_claims(code,device_id,created_at) VALUES(?,?,?)",
                       (code, device_id, now_iso()))
        return token

    def authenticate(self, device_id: str, token: str) -> bool:
        with self.connect() as db:
            row = db.execute("SELECT token_hash FROM devices WHERE device_id=?", (device_id,)).fetchone()
        return bool(row and secrets.compare_digest(row["token_hash"], self.hash_token(token)))

    def heartbeat(self, device_id: str, data: dict[str, Any]) -> None:
        with self.connect() as db:
            db.execute(
                "UPDATE devices SET firmware_version=?, last_seen_at=?, battery_voltage=?, rssi=?, runtime_mode=? WHERE device_id=?",
                (data["firmware_version"], now_iso(), data.get("battery_voltage"), data.get("rssi"), data["runtime_mode"], device_id),
            )

    def capabilities(self, device_id: str) -> dict[str, Any] | None:
        with self.connect() as db:
            row = db.execute("SELECT capabilities FROM devices WHERE device_id=?", (device_id,)).fetchone()
        return json.loads(row["capabilities"]) if row else None

    def create_task(self, device_id: str, payload: dict[str, Any], expires: int,
                    source: str = "openapi", created_by: str | None = None) -> str | None:
        task_id = secrets.token_hex(12)
        with self.connect() as db:
            exists = db.execute("SELECT 1 FROM devices WHERE device_id=?", (device_id,)).fetchone()
            if not exists:
                return None
            expires_at = (datetime.now(UTC) + timedelta(seconds=expires)).isoformat()
            db.execute("INSERT INTO tasks(id,device_id,payload,status,created_at,expires_at,source,created_by) VALUES(?,?,?,?,?,?,?,?)",
                       (task_id, device_id, json.dumps(payload, ensure_ascii=False), "queued", now_iso(), expires_at, source, created_by))
        return task_id

    def next_task(self, device_id: str) -> dict[str, Any] | None:
        with self.connect() as db:
            row = db.execute(
                "SELECT * FROM tasks WHERE device_id=? AND status='queued' AND expires_at>? ORDER BY created_at LIMIT 1",
                (device_id, now_iso()),
            ).fetchone()
            if not row:
                return None
        return {"id": row["id"], "payload": json.loads(row["payload"]), "created_at": row["created_at"], "expires_at": row["expires_at"]}

    def ack(self, device_id: str, task_id: str, status: str, detail: str | None) -> bool:
        with self.connect() as db:
            cur = db.execute("UPDATE tasks SET status=?, acknowledged_at=?, detail=? WHERE id=? AND device_id=?",
                             (status, now_iso(), detail, task_id, device_id))
        return cur.rowcount == 1

    def bind_demo_device(self, user_id: str, device_id: str, nickname: str = "我的小悟") -> None:
        with self.connect() as db:
            db.execute("INSERT OR IGNORE INTO device_bindings VALUES(?,?,?)", (user_id, device_id, nickname))

    def user_devices(self, user_id: str) -> list[dict[str, Any]]:
        with self.connect() as db:
            rows = db.execute(
                "SELECT d.device_id,d.model,d.firmware_version,d.last_seen_at,d.battery_voltage,d.rssi,b.nickname "
                "FROM device_bindings b JOIN devices d ON d.device_id=b.device_id WHERE b.user_id=? ORDER BY b.nickname",
                (user_id,),
            ).fetchall()
        return [dict(row) for row in rows]

    def is_platform_admin(self, user_id: str) -> bool:
        """是否平台管理员（admin_accounts 里启用中的账号）。

        管理员与普通用户是两套身份：管理员不参与 device_bindings，
        因此设备列表要单独走「全部设备」视图。
        """
        with self.connect() as db:
            row = db.execute(
                "SELECT 1 FROM admin_accounts WHERE admin_id=? AND status='active'", (user_id,)
            ).fetchone()
        return row is not None

    def all_devices_with_owner(self) -> list[dict[str, Any]]:
        """管理员视角：全部设备 + 归属账号（未绑定为 None）。"""
        with self.connect() as db:
            rows = db.execute(
                "SELECT d.device_id, d.model, d.firmware_version, d.last_seen_at, d.battery_voltage, d.rssi,"
                " (SELECT b.nickname FROM device_bindings b WHERE b.device_id=d.device_id ORDER BY b.rowid LIMIT 1) AS nickname,"
                " (SELECT b.user_id  FROM device_bindings b WHERE b.device_id=d.device_id ORDER BY b.rowid LIMIT 1) AS owner_user_id,"
                " (SELECT u.username FROM device_bindings b JOIN users u ON u.user_id=b.user_id"
                "   WHERE b.device_id=d.device_id ORDER BY b.rowid LIMIT 1) AS owner_username"
                " FROM devices d"
                " ORDER BY d.last_seen_at IS NULL, d.last_seen_at DESC, d.device_id"
            ).fetchall()
        return [dict(row) for row in rows]

    def device_members(self, device_id: str) -> list[dict[str, Any]]:
        with self.connect() as db:
            rows = db.execute(
                "SELECT b.user_id,u.username,b.nickname FROM device_bindings b JOIN users u ON u.user_id=b.user_id "
                "WHERE b.device_id=? ORDER BY b.nickname",
                (device_id,),
            ).fetchall()
        return [dict(row) for row in rows]

    def remove_member(self, device_id: str, user_id: str) -> None:
        with self.connect() as db:
            db.execute("DELETE FROM device_bindings WHERE user_id=? AND device_id=?", (user_id, device_id))

    def user_owns(self, user_id: str, device_id: str) -> bool:
        with self.connect() as db:
            return db.execute("SELECT 1 FROM device_bindings WHERE user_id=? AND device_id=?", (user_id, device_id)).fetchone() is not None

    def device_owner(self, device_id: str) -> str | None:
        """返回设备当前绑定的账号（未绑定为 None）。"""
        with self.connect() as db:
            row = db.execute("SELECT user_id FROM device_bindings WHERE device_id=?", (device_id,)).fetchone()
        return row["user_id"] if row else None

    # ── 确权层：配对码（pairing code）+ 应用-设备授权（grant）────────────────
    # 平台只回答「这条数据可以显示在哪块屏」：由已登录用户一次性确认，建立
    # app_id + device_id + user_id 三元授权，全程不接触任何大模型凭证。
    PAIRING_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"

    @staticmethod
    def _normalize_pairing_code(code: str) -> str:
        return (code or "").replace("-", "").replace(" ", "").strip().upper()

    def create_pairing_code(self, app_id: str, developer_id: str, label: str,
                            scopes: list[str], ttl_seconds: int = 600) -> dict[str, Any]:
        """为应用签发一次性绑定码（默认 10 分钟有效）。"""
        code = ""
        with self.connect() as db:
            for _ in range(20):
                candidate = "".join(secrets.choice(self.PAIRING_ALPHABET) for _ in range(6))
                if not db.execute("SELECT 1 FROM pairing_codes WHERE code=?", (candidate,)).fetchone():
                    code = candidate
                    break
            if not code:
                raise RuntimeError("配对码生成失败：可用空间耗尽")
            created = datetime.now(UTC)
            expires = created + timedelta(seconds=ttl_seconds)
            db.execute(
                "INSERT INTO pairing_codes(code,app_id,developer_id,label,scopes,status,created_at,expires_at) "
                "VALUES(?,?,?,?,?,'pending',?,?)",
                (code, app_id, developer_id, label, json.dumps(scopes, ensure_ascii=False),
                 created.isoformat(), expires.isoformat()),
            )
        return {"code": code, "display_code": f"{code[:3]}-{code[3:]}",
                "expires_in": ttl_seconds, "expires_at": expires.isoformat()}

    def get_pairing_code(self, code: str) -> dict[str, Any] | None:
        """读取配对码；处于 pending 且已过期的，顺手落为 expired。"""
        key = self._normalize_pairing_code(code)
        if not key:
            return None
        with self.connect() as db:
            row = db.execute("SELECT * FROM pairing_codes WHERE code=?", (key,)).fetchone()
            if not row:
                return None
            item = dict(row)
            item["scopes"] = json.loads(item["scopes"] or "[]")
            if item["status"] == "pending" and item["expires_at"] < now_iso():
                db.execute("UPDATE pairing_codes SET status='expired' WHERE code=?", (key,))
                item["status"] = "expired"
        return item

    def upsert_grant(self, app_id: str, device_id: str, user_id: str,
                     scopes: list[str], label: str) -> dict[str, Any]:
        """建立（或恢复）一条 app↔device↔user 授权。撤销后重新授权会复用同一行。"""
        grant_id, now = "grant-" + secrets.token_hex(6), now_iso()
        with self.connect() as db:
            db.execute(
                "INSERT INTO app_device_grants(grant_id,app_id,device_id,user_id,scopes,label,status,created_at,revoked_at,last_push_at) "
                "VALUES(?,?,?,?,?,?,'active',?,NULL,NULL) "
                "ON CONFLICT(app_id,device_id) DO UPDATE SET grant_id=excluded.grant_id,user_id=excluded.user_id,"
                "scopes=excluded.scopes,label=excluded.label,status='active',created_at=excluded.created_at,revoked_at=NULL",
                (grant_id, app_id, device_id, user_id, json.dumps(scopes, ensure_ascii=False), label, now),
            )
        return {"grant_id": grant_id, "app_id": app_id, "device_id": device_id, "user_id": user_id,
                "scopes": scopes, "label": label, "status": "active", "created_at": now}

    def approve_pairing_code(self, code: str, user_id: str, device_id: str) -> dict[str, Any]:
        """用户确认授权。抛 LookupError/TimeoutError/ValueError/PermissionError 供上层转 HTTP。"""
        item = self.get_pairing_code(code)
        if not item:
            raise LookupError("pairing_not_found")
        if item["status"] == "expired":
            raise TimeoutError("pairing_expired")
        if item["status"] in ("approved", "denied"):
            raise ValueError("pairing_already_used")
        if not self.user_owns(user_id, device_id):
            raise PermissionError("device_not_owned")
        with self.connect() as db:
            row = db.execute("SELECT scopes FROM developer_apps WHERE app_id=?", (item["app_id"],)).fetchone()
        owned_scopes = set(json.loads(row["scopes"])) if row and row["scopes"] else set()
        scopes = [s for s in item["scopes"] if s in owned_scopes]
        if not scopes:
            raise PermissionError("no_grantable_scope")
        grant = self.upsert_grant(item["app_id"], device_id, user_id, scopes, item["label"])
        with self.connect() as db:
            db.execute("UPDATE pairing_codes SET status='approved',device_id=?,user_id=?,grant_id=? WHERE code=?",
                       (device_id, user_id, grant["grant_id"], item["code"]))
        return grant

    def deny_pairing_code(self, code: str, user_id: str) -> bool:
        key = self._normalize_pairing_code(code)
        with self.connect() as db:
            cur = db.execute("UPDATE pairing_codes SET status='denied',user_id=? WHERE code=? AND status='pending'",
                             (user_id, key))
            return cur.rowcount > 0

    def get_active_grant(self, app_id: str, device_id: str) -> dict[str, Any] | None:
        """推送前的授权校验：该应用对该设备是否仍有有效授权。"""
        cands = _mac_candidates(device_id)
        if not cands:
            return None
        marks = ",".join("?" for _ in cands)
        with self.connect() as db:
            row = db.execute(
                f"SELECT * FROM app_device_grants WHERE app_id=? AND status='active' AND device_id IN ({marks})",
                (app_id, *cands)).fetchone()
        if not row:
            return None
        item = dict(row)
        item["scopes"] = json.loads(item["scopes"] or "[]")
        return item

    def list_user_grants(self, user_id: str) -> list[dict[str, Any]]:
        with self.connect() as db:
            rows = db.execute(
                "SELECT g.*, a.name AS app_name FROM app_device_grants g "
                "LEFT JOIN developer_apps a ON a.app_id=g.app_id "
                "WHERE g.user_id=? ORDER BY g.created_at DESC", (user_id,)).fetchall()
        out: list[dict[str, Any]] = []
        for row in rows:
            item = dict(row)
            item["scopes"] = json.loads(item["scopes"] or "[]")
            out.append(item)
        return out

    def revoke_grant(self, grant_id: str, user_id: str | None = None) -> bool:
        with self.connect() as db:
            if user_id:
                cur = db.execute("UPDATE app_device_grants SET status='revoked',revoked_at=? "
                                 "WHERE grant_id=? AND user_id=?", (now_iso(), grant_id, user_id))
            else:
                cur = db.execute("UPDATE app_device_grants SET status='revoked',revoked_at=? WHERE grant_id=?",
                                 (now_iso(), grant_id))
            return cur.rowcount > 0

    def touch_grant_push(self, grant_id: str) -> None:
        with self.connect() as db:
            db.execute("UPDATE app_device_grants SET last_push_at=? WHERE grant_id=?", (now_iso(), grant_id))

    def list_templates(self, enabled_only: bool = False) -> list[dict[str, Any]]:
        sql = "SELECT * FROM templates" + (" WHERE enabled=1" if enabled_only else "") + " ORDER BY category,name"
        with self.connect() as db:
            rows = db.execute(sql).fetchall()
        return [{**dict(row), "enabled": bool(row["enabled"]), "schema_definition": json.loads(row["schema_definition"])} for row in rows]

    def upsert_template(self, item: dict[str, Any], actor: str) -> None:
        with self.connect() as db:
            db.execute(
                "INSERT INTO templates VALUES(?,?,?,?,?,?) ON CONFLICT(template_id) DO UPDATE SET "
                "name=excluded.name,category=excluded.category,schema_definition=excluded.schema_definition,enabled=excluded.enabled,updated_at=excluded.updated_at",
                (item["template_id"], item["name"], item["category"], json.dumps(item["schema_definition"], ensure_ascii=False), int(item["enabled"]), now_iso()),
            )
        self.audit(actor, "template.upsert", item["template_id"], {"enabled": item["enabled"]})

    def task_history(self, device_id: str | None = None, limit: int = 100) -> list[dict[str, Any]]:
        sql, params = "SELECT * FROM tasks", []
        if device_id:
            sql, params = sql + " WHERE device_id=?", [device_id]
        sql += " ORDER BY created_at DESC LIMIT ?"
        params.append(limit)
        with self.connect() as db:
            rows = db.execute(sql, params).fetchall()
        return [{**dict(row), "payload": json.loads(row["payload"])} for row in rows]

    def all_devices(self) -> list[dict[str, Any]]:
        with self.connect() as db:
            rows = db.execute("SELECT device_id,model,firmware_version,created_at,last_seen_at,battery_voltage,rssi,runtime_mode FROM devices ORDER BY created_at DESC").fetchall()
        return [dict(row) for row in rows]

    def overview(self) -> dict[str, int]:
        with self.connect() as db:
            devices = db.execute("SELECT COUNT(*) FROM devices").fetchone()[0]
            online = db.execute("SELECT COUNT(*) FROM devices WHERE last_seen_at IS NOT NULL").fetchone()[0]
            queued = db.execute("SELECT COUNT(*) FROM tasks WHERE status='queued'").fetchone()[0]
            displayed = db.execute("SELECT COUNT(*) FROM tasks WHERE status='displayed'").fetchone()[0]
        return {"devices": devices, "seen_devices": online, "queued_tasks": queued, "displayed_tasks": displayed}

    def audit(self, actor: str, action: str, target: str, detail: dict[str, Any]) -> None:
        with self.connect() as db:
            db.execute("INSERT INTO audit_logs(actor,action,target,detail,created_at) VALUES(?,?,?,?,?)",
                       (actor, action, target, json.dumps(detail, ensure_ascii=False), now_iso()))

    def audits(self, limit: int = 100) -> list[dict[str, Any]]:
        with self.connect() as db:
            rows = db.execute("SELECT * FROM audit_logs ORDER BY id DESC LIMIT ?", (limit,)).fetchall()
        return [{**dict(row), "detail": json.loads(row["detail"])} for row in rows]

    @staticmethod
    def _password_hash(password: str, salt: str) -> str:
        return hashlib.pbkdf2_hmac("sha256", password.encode(), bytes.fromhex(salt), 120_000).hex()

    def _ensure_demo_user(self, db: sqlite3.Connection) -> None:
        salt = "7869616f77752d73332d64656d6f2d31"
        password_hash = self._password_hash("demo1234", salt)
        db.execute("INSERT OR IGNORE INTO users VALUES(?,?,?,?,?)", ("demo-user", "demo", password_hash, salt, now_iso()))
        db.execute("INSERT OR IGNORE INTO user_sessions VALUES(?,?,?)",
                   (self.hash_token("demo-user-token"), "demo-user", "2099-01-01T00:00:00+00:00"))

    def _ensure_demo_app(self, db: sqlite3.Connection) -> None:
        db.execute(
            "INSERT OR IGNORE INTO developer_apps VALUES(?,?,?,?,?,?)",
            ("app-dev", "Local development", self.hash_token("dev-app-key"),
             json.dumps(["device.content.write", "device.read", "device.event.read", "webhook.manage"]),
             "active", now_iso()),
        )

    def ensure_admin(self, username: str, password: str) -> None:
        """Create or refresh the configured local administrator from environment settings."""
        username = username.strip()
        if not username or not password:
            return
        salt = secrets.token_hex(16)
        admin_id = "admin-" + secrets.token_hex(8)
        with self.connect() as db:
            db.execute(
                "INSERT INTO admin_accounts(admin_id,username,password_hash,password_salt,display_name,status,created_at) "
                "VALUES(?,?,?,?,?,'active',?) ON CONFLICT(username) DO UPDATE SET "
                "password_hash=excluded.password_hash,password_salt=excluded.password_salt,status='active'",
                (admin_id, username, self._password_hash(password, salt), salt, "平台管理员", now_iso()),
            )

    def login_admin(self, username: str, password: str) -> tuple[dict[str, Any], str] | None:
        with self.connect() as db:
            row = db.execute("SELECT * FROM admin_accounts WHERE username=?", (username.strip(),)).fetchone()
            if not row or row["status"] != "active" or not secrets.compare_digest(
                    row["password_hash"], self._password_hash(password, row["password_salt"])):
                return None
            token = secrets.token_urlsafe(32)
            expires = (datetime.now(UTC) + timedelta(hours=12)).isoformat()
            db.execute("INSERT INTO admin_sessions VALUES(?,?,?)", (self.hash_token(token), row["admin_id"], expires))
        return {"admin_id": row["admin_id"], "username": row["username"], "display_name": row["display_name"]}, token

    def admin_by_session(self, token: str) -> dict[str, Any] | None:
        with self.connect() as db:
            row = db.execute(
                "SELECT a.admin_id,a.username,a.display_name FROM admin_sessions s "
                "JOIN admin_accounts a ON a.admin_id=s.admin_id "
                "WHERE s.token_hash=? AND s.expires_at>? AND a.status='active'",
                (self.hash_token(token), now_iso()),
            ).fetchone()
        return dict(row) if row else None

    def list_developer_accounts(self) -> list[dict[str, Any]]:
        with self.connect() as db:
            rows = db.execute(
                "SELECT developer_id,name,organization,role,email,agreement_version,"
                "agreement_accepted_at,status,created_at FROM developer_accounts ORDER BY created_at DESC"
            ).fetchall()
        return [dict(row) for row in rows]

    def login(self, username: str, password: str) -> tuple[str, str] | None:
        with self.connect() as db:
            row = db.execute("SELECT * FROM users WHERE username=?", (username,)).fetchone()
            if row and secrets.compare_digest(row["password_hash"], self._password_hash(password, row["password_salt"])):
                token = secrets.token_urlsafe(32)
                expires = (datetime.now(UTC) + timedelta(days=30)).isoformat()
                db.execute("INSERT INTO user_sessions VALUES(?,?,?)", (self.hash_token(token), row["user_id"], expires))
                return row["user_id"], token
            # 管理员账号（admin_accounts）也可在内容后台登录，原后台体系共用
            arow = db.execute("SELECT * FROM admin_accounts WHERE username=?", (username.strip(),)).fetchone()
            if arow and arow["status"] == "active" and secrets.compare_digest(
                    arow["password_hash"], self._password_hash(password, arow["password_salt"])):
                token = secrets.token_urlsafe(32)
                expires = (datetime.now(UTC) + timedelta(days=30)).isoformat()
                db.execute("INSERT INTO user_sessions VALUES(?,?,?)", (self.hash_token(token), arow["admin_id"], expires))
                return arow["admin_id"], token
        return None

    def session_user(self, token: str) -> str | None:
        with self.connect() as db:
            row = db.execute("SELECT user_id FROM user_sessions WHERE token_hash=? AND expires_at>?",
                             (self.hash_token(token), now_iso())).fetchone()
        return row["user_id"] if row else None

    def create_session_token(self, user_id: str) -> str:
        token = secrets.token_urlsafe(32)
        expires = (datetime.now(UTC) + timedelta(days=30)).isoformat()
        with self.connect() as db:
            db.execute("INSERT INTO user_sessions VALUES(?,?,?)", (self.hash_token(token), user_id, expires))
        return token

    def device_exists(self, device_id: str) -> bool:
        with self.connect() as db:
            return db.execute("SELECT 1 FROM devices WHERE device_id=?", (device_id,)).fetchone() is not None

    def get_content_config(self, device_id: str) -> dict[str, Any] | None:
        with self.connect() as db:
            for cand in _mac_candidates(device_id):
                row = db.execute(
                    "SELECT payload, updated_at FROM content_configs WHERE device_id=?",
                    (cand,),
                ).fetchone()
                if row:
                    return {"config": json.loads(row["payload"]), "updated_at": row["updated_at"]}
        return None

    def get_device_owner(self, device_id: str) -> str | None:
        """返回设备绑定关系的 owner（首个绑定用户），未绑定返回 None。"""
        with self.connect() as db:
            row = db.execute(
                "SELECT user_id FROM device_bindings WHERE device_id=? ORDER BY rowid ASC LIMIT 1",
                (device_id,),
            ).fetchone()
        return row["user_id"] if row else None

    def get_claim_code(self, device_id: str) -> str | None:
        """返回设备配对码（用于未绑定引导图）。"""
        with self.connect() as db:
            row = db.execute("SELECT code FROM device_claims WHERE device_id=?", (device_id,)).fetchone()
        return row["code"] if row else None

    def ensure_claim_code(self, device_id: str) -> str:
        """保证设备一定有一个配对码：没有就现生成一个。

        老设备（或早期注册路径）可能只有 devices 行没有 device_claims 行，
        屏幕会显示「------」，用户永远绑不上。这里兜底补发。
        """
        with self.connect() as db:
            row = db.execute("SELECT code FROM device_claims WHERE device_id=?", (device_id,)).fetchone()
            if row:
                return row["code"]
            code = secrets.token_hex(3).upper()
            db.execute("INSERT OR REPLACE INTO device_claims(code,device_id,created_at) VALUES(?,?,?)",
                       (code, device_id, now_iso()))
        return code

    def consume_pending_refresh(self, device_id: str) -> bool:
        """若设备有待刷新标记则清除并返回 True（设备已拉取）。"""
        with self.connect() as db:
            row = db.execute("SELECT pending_refresh FROM devices WHERE device_id=?", (device_id,)).fetchone()
            was = bool(row and int(row["pending_refresh"]))
            if was:
                db.execute("UPDATE devices SET pending_refresh=0 WHERE device_id=?", (device_id,))
        return was

    def save_content_config(self, device_id: str, payload: dict[str, Any]) -> str:
        ts = now_iso()
        with self.connect() as db:
            db.execute(
                "INSERT INTO content_configs(device_id,payload,updated_at) VALUES(?,?,?) "
                "ON CONFLICT(device_id) DO UPDATE SET payload=excluded.payload, updated_at=excluded.updated_at",
                (device_id, json.dumps(payload, ensure_ascii=False), ts),
            )
        return ts

    # —— 用户内容后台（dual 控制台）所需 ——
    def register_user(self, username: str, password: str) -> tuple[str, str]:
        if not username or not password:
            raise ValueError("用户名和密码不能为空")
        if len(password) < 6:
            raise ValueError("密码至少 6 位")
        salt = secrets.token_hex(16)
        with self.connect() as db:
            if db.execute("SELECT 1 FROM users WHERE username=?", (username,)).fetchone():
                raise ValueError("用户名已被注册")
            user_id = "u-" + secrets.token_hex(8)
            db.execute("INSERT INTO users VALUES(?,?,?,?,?)",
                       (user_id, username, self._password_hash(password, salt), salt, now_iso()))
            token = secrets.token_urlsafe(32)
            expires = (datetime.now(UTC) + timedelta(days=30)).isoformat()
            db.execute("INSERT INTO user_sessions VALUES(?,?,?)", (self.hash_token(token), user_id, expires))
        return user_id, token

    def user_id_by_username(self, username: str) -> str | None:
        with self.connect() as db:
            row = db.execute("SELECT user_id FROM users WHERE username=?", (username,)).fetchone()
        return row["user_id"] if row else None

    def bind_device(self, user_id: str, device_id: str, nickname: str) -> None:
        if not self.device_exists(device_id):
            raise ValueError("设备不存在，请先开机配网注册")
        with self.connect() as db:
            db.execute("INSERT OR REPLACE INTO device_bindings VALUES(?,?,?)", (user_id, device_id, nickname))

    def unbind_device(self, user_id: str, device_id: str) -> None:
        with self.connect() as db:
            db.execute("DELETE FROM device_bindings WHERE user_id=? AND device_id=?", (user_id, device_id))

    def share_device(self, owner_id: str, device_id: str, target_username: str, role: str) -> None:
        target = self.user_id_by_username(target_username)
        if not target:
            raise ValueError("对方账号不存在")
        if target == owner_id:
            raise ValueError("不能分享给自己")
        with self.connect() as db:
            existing = db.execute("SELECT nickname FROM device_bindings WHERE user_id=? AND device_id=?",
                                 (owner_id, device_id)).fetchone()
            nickname = existing["nickname"] if existing else "共享设备"
            db.execute("INSERT OR REPLACE INTO device_bindings VALUES(?,?,?)", (target, device_id, nickname))

    def consume_claim(self, code: str, user_id: str, nickname: str) -> str:
        with self.connect() as db:
            row = db.execute("SELECT device_id FROM device_claims WHERE code=?", (code.strip().upper(),)).fetchone()
            if not row:
                raise ValueError("配对码无效或已失效")
            device_id = row["device_id"]
            db.execute("INSERT OR REPLACE INTO device_bindings VALUES(?,?,?)", (user_id, device_id, nickname or "我的小悟双屏"))
        return device_id

    def set_teacher_auth(self, device_id: str, frozen: bool, actor: str) -> None:
        with self.connect() as db:
            db.execute(
                "INSERT INTO teacher_auth(device_id,frozen,frozen_by,frozen_at) VALUES(?,?,?,?) "
                "ON CONFLICT(device_id) DO UPDATE SET frozen=excluded.frozen, frozen_by=excluded.frozen_by, frozen_at=excluded.frozen_at",
                (device_id, int(bool(frozen)), actor, now_iso()),
            )

    def get_teacher_auth(self, device_id: str) -> dict[str, Any] | None:
        with self.connect() as db:
            row = db.execute("SELECT * FROM teacher_auth WHERE device_id=?", (device_id,)).fetchone()
        return dict(row) if row else None

    def get_user_profile(self, user_id: str, category: str) -> dict[str, Any] | None:
        with self.connect() as db:
            row = db.execute("SELECT data FROM user_profiles WHERE user_id=? AND category=?", (user_id, category)).fetchone()
        return json.loads(row["data"]) if row else None

    def set_user_profile(self, user_id: str, category: str, data: dict[str, Any]) -> None:
        with self.connect() as db:
            db.execute(
                "INSERT INTO user_profiles(user_id,category,data,updated_at) VALUES(?,?,?,?) "
                "ON CONFLICT(user_id,category) DO UPDATE SET data=excluded.data, updated_at=excluded.updated_at",
                (user_id, category, json.dumps(data, ensure_ascii=False), now_iso()),
            )

    def get_service_config(self) -> dict[str, Any]:
        with self.connect() as db:
            row = db.execute("SELECT value FROM service_configs WHERE key='service_config'").fetchone()
        return json.loads(row["value"]) if row else {}

    def set_service_config(self, data: dict[str, Any]) -> None:
        with self.connect() as db:
            db.execute(
                "INSERT INTO service_configs(key,value,updated_at) VALUES(?,?,?) "
                "ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=excluded.updated_at",
                ("service_config", json.dumps(data, ensure_ascii=False), now_iso()),
            )

    def create_teacher_code(self, device_id: str, auth_name: str, permission: str, created_by: str, validity_days: int) -> dict[str, Any]:
        import secrets as _secrets
        code = _secrets.token_hex(3).upper()
        expire_at = None
        if validity_days and validity_days > 0:
            expire_at = (datetime.now(UTC) + timedelta(days=validity_days)).isoformat()
        with self.connect() as db:
            cur = db.execute(
                "INSERT INTO teacher_codes(device_id,auth_code,auth_name,permission,created_by,created_at,expire_at,frozen) VALUES(?,?,?,?,?,?,?,0)",
                (device_id, code, auth_name, permission, created_by, now_iso(), expire_at),
            )
            rid = cur.lastrowid
            row = db.execute("SELECT * FROM teacher_codes WHERE id=?", (rid,)).fetchone()
        return self._teacher_row(dict(row))

    def list_teacher_codes(self, device_id: str) -> list[dict[str, Any]]:
        with self.connect() as db:
            rows = db.execute("SELECT * FROM teacher_codes WHERE device_id=? ORDER BY id DESC", (device_id,)).fetchall()
        return [self._teacher_row(dict(r)) for r in rows]

    def freeze_teacher_code(self, device_id: str, auth_code: str, frozen: bool) -> None:
        with self.connect() as db:
            db.execute("UPDATE teacher_codes SET frozen=? WHERE device_id=? AND auth_code=?",
                       (int(bool(frozen)), device_id, auth_code))

    @staticmethod
    def _teacher_row(r: dict[str, Any]) -> dict[str, Any]:
        expired = bool(r["expire_at"]) and r["expire_at"] < now_iso()
        return {
            "id": r["id"], "auth_code": r["auth_code"], "auth_name": r["auth_name"],
            "permission": r["permission"], "created_at": r["created_at"], "expire_time": r["expire_at"],
            "is_frozen": bool(r["frozen"]), "expired": expired,
        }

    # ——— 微信用户（小程序一键登录，openid↔账号映射）———
    def get_or_create_wechat_user(self, openid: str) -> tuple[str, str, bool]:
        with self.connect() as db:
            row = db.execute("SELECT user_id FROM wechat_openid_map WHERE openid=?", (openid,)).fetchone()
            if row:
                uid = row["user_id"]
                u = db.execute("SELECT username FROM users WHERE user_id=?", (uid,)).fetchone()
                return uid, (u["username"] if u else ("wx_" + openid[-12:])), False
            username = "wx_" + openid[-12:]
            uid = secrets.token_hex(8)
            pwd = secrets.token_urlsafe(16)
            db.execute("INSERT OR REPLACE INTO users VALUES(?,?,?,?,?)",
                       (uid, username, self._password_hash(pwd, secrets.token_hex(8)), secrets.token_hex(8), now_iso()))
            db.execute("INSERT OR REPLACE INTO wechat_openid_map(openid,user_id,created_at) VALUES(?,?,?)",
                       (openid, uid, now_iso()))
            return uid, username, True

    # ——— 推送记录（老师快捷推送作业/便签）———
    def save_push(self, device_id: str, ptype: str, sender_type: str, sender_name: str,
                  content: str, is_append: bool, count: int) -> None:
        with self.connect() as db:
            db.execute(
                "INSERT INTO push_record(device_id,push_type,sender_type,sender_name,content,is_append,item_count,create_time) "
                "VALUES(?,?,?,?,?,?,?,?)",
                (device_id, ptype, sender_type, sender_name, content[:2000], int(bool(is_append)), count, now_iso()),
            )

    def get_push_history(self, device_id: str, limit: int = 200) -> list[dict[str, Any]]:
        with self.connect() as db:
            rows = db.execute(
                "SELECT id,push_type,sender_type,sender_name,content,is_append,item_count,create_time "
                "FROM push_record WHERE device_id=? ORDER BY id DESC LIMIT ?", (device_id, limit)
            ).fetchall()
        return [dict(r) for r in rows]

    def delete_push(self, device_id: str, push_id: int) -> bool:
        with self.connect() as db:
            cur = db.execute("DELETE FROM push_record WHERE device_id=? AND id=?", (device_id, push_id))
            return cur.rowcount > 0

    # ——— pending_refresh（A 屏立即同步信号）———
    def set_pending_refresh(self, device_id: str, value: bool) -> None:
        with self.connect() as db:
            db.execute("UPDATE devices SET pending_refresh=? WHERE device_id=?", (int(bool(value)), device_id))

    def get_pending_refresh(self, device_id: str) -> int:
        with self.connect() as db:
            row = db.execute("SELECT pending_refresh FROM devices WHERE device_id=?", (device_id,)).fetchone()
        return int(row["pending_refresh"]) if row else 0

    def template(self, template_id: str) -> dict[str, Any] | None:
        with self.connect() as db:
            row = db.execute("SELECT * FROM templates WHERE template_id=? AND enabled=1", (template_id,)).fetchone()
        if not row:
            return None
        return {**dict(row), "schema_definition": json.loads(row["schema_definition"])}

    def create_developer_app(self, name: str, scopes: list[str]) -> tuple[str, str]:
        app_id, token = "app-" + secrets.token_hex(6), secrets.token_urlsafe(36)
        with self.connect() as db:
            db.execute("INSERT INTO developer_apps VALUES(?,?,?,?,?,?)",
                       (app_id, name, self.hash_token(token), json.dumps(scopes), "active", now_iso()))
        return app_id, token

    def register_developer(self, name: str, organization: str, email: str, password: str,
                           agreement_version: str, role: str = "developer") -> tuple[str, str]:
        email = email.strip().lower()
        salt = secrets.token_hex(16)
        developer_id = "dev-" + secrets.token_hex(6)
        accepted_at = now_iso()
        with self.connect() as db:
            if db.execute("SELECT 1 FROM developer_accounts WHERE email=?", (email,)).fetchone():
                raise ValueError("该邮箱已注册")
            db.execute("INSERT INTO developer_accounts "
                       "(developer_id,name,organization,role,email,password_hash,password_salt,agreement_version,agreement_accepted_at,status,created_at) "
                       "VALUES(?,?,?,?,?,?,?,?,?,?,?)", (
                developer_id, name.strip(), organization.strip(), role, email,
                self._password_hash(password, salt), salt, agreement_version, accepted_at, "active", accepted_at,
            ))
        return developer_id, self.create_developer_session(developer_id)

    def create_developer_session(self, developer_id: str) -> str:
        token = secrets.token_urlsafe(32)
        expires = (datetime.now(UTC) + timedelta(days=30)).isoformat()
        with self.connect() as db:
            db.execute("INSERT INTO developer_sessions VALUES(?,?,?)", (self.hash_token(token), developer_id, expires))
        return token

    def login_developer(self, email: str, password: str) -> tuple[str, str] | None:
        with self.connect() as db:
            row = db.execute("SELECT * FROM developer_accounts WHERE email=?", (email.strip().lower(),)).fetchone()
        if not row or row["status"] != "active" or not secrets.compare_digest(
                row["password_hash"], self._password_hash(password, row["password_salt"])):
            return None
        return row["developer_id"], self.create_developer_session(row["developer_id"])

    def developer_by_session(self, token: str) -> dict[str, Any] | None:
        with self.connect() as db:
            row = db.execute(
                "SELECT a.developer_id,a.name,a.organization,a.role,a.email,a.agreement_version,a.status,a.created_at "
                "FROM developer_sessions s JOIN developer_accounts a ON a.developer_id=s.developer_id "
                "WHERE s.token_hash=? AND s.expires_at>?", (self.hash_token(token), now_iso()),
            ).fetchone()
        return dict(row) if row and row["status"] == "active" else None

    def create_sandbox_app(self, developer_id: str, name: str, scene: str, description: str) -> tuple[dict[str, Any], str]:
        app_id, token, ts = "sand-" + secrets.token_hex(6), secrets.token_urlsafe(32), now_iso()
        with self.connect() as db:
            db.execute("INSERT INTO sandbox_apps VALUES(?,?,?,?,?,?,?,?,?,?,?)", (
                app_id, developer_id, name, scene, description, "sandbox", self.hash_token(token),
                "0.1.0", "", ts, ts,
            ))
        return self.sandbox_app(app_id, developer_id), token

    def sandbox_app(self, app_id: str, developer_id: str | None = None) -> dict[str, Any] | None:
        sql, params = "SELECT app_id,developer_id,name,scene,description,status,version,review_note,created_at,updated_at FROM sandbox_apps WHERE app_id=?", [app_id]
        if developer_id:
            sql, params = sql + " AND developer_id=?", [app_id, developer_id]
        with self.connect() as db:
            row = db.execute(sql, params).fetchone()
        return dict(row) if row else None

    def list_sandbox_apps(self, developer_id: str | None = None) -> list[dict[str, Any]]:
        sql, params = "SELECT app_id,developer_id,name,scene,description,status,version,review_note,created_at,updated_at FROM sandbox_apps", []
        if developer_id:
            sql, params = sql + " WHERE developer_id=?", [developer_id]
        sql += " ORDER BY updated_at DESC"
        with self.connect() as db:
            return [dict(row) for row in db.execute(sql, params).fetchall()]

    def save_sandbox_build(self, app_id: str, build: dict[str, Any]) -> None:
        report = build.get("report") or {}
        with self.connect() as db:
            db.execute(
                "INSERT INTO sandbox_builds (app_id,repo_url,repo_slug,branch,commit_sha,entry,verdict,report,"
                "file_count,size_bytes,imported_at) VALUES(?,?,?,?,?,?,?,?,?,?,?) "
                "ON CONFLICT(app_id) DO UPDATE SET repo_url=excluded.repo_url,repo_slug=excluded.repo_slug,"
                "branch=excluded.branch,commit_sha=excluded.commit_sha,entry=excluded.entry,verdict=excluded.verdict,"
                "report=excluded.report,file_count=excluded.file_count,size_bytes=excluded.size_bytes,"
                "imported_at=excluded.imported_at",
                (app_id, build.get("repo_url", ""), build.get("repo_slug", ""), build.get("branch", ""),
                 build.get("commit_sha", ""), build.get("entry", ""), report.get("verdict", "pass"),
                 json.dumps(report, ensure_ascii=False), int(build.get("file_count") or 0),
                 int(build.get("size_bytes") or 0), now_iso()),
            )

    def get_sandbox_build(self, app_id: str) -> dict[str, Any] | None:
        with self.connect() as db:
            row = db.execute(
                "SELECT app_id,repo_url,repo_slug,branch,commit_sha,entry,verdict,report,file_count,"
                "size_bytes,imported_at FROM sandbox_builds WHERE app_id=?", (app_id,),
            ).fetchone()
        if not row:
            return None
        result = dict(row)
        try:
            result["report"] = json.loads(result["report"] or "{}")
        except json.JSONDecodeError:
            result["report"] = {}
        return result

    # —— 轨2：数据接口（ISV 提供端点，后端实时渲染 e-ink 卡片）——
    def save_app_data_source(self, app_id: str, endpoint: str, token: str,
                             verdict: str, checks: list[dict[str, Any]],
                             sample: dict[str, Any] | None, last_error: str = "") -> None:
        ts = now_iso()
        with self.connect() as db:
            db.execute(
                "INSERT INTO app_data_sources(app_id,endpoint,token,sample_json,verdict,checks_json,"
                "checked_at,last_error,updated_at) VALUES(?,?,?,?,?,?,?,?,?) "
                "ON CONFLICT(app_id) DO UPDATE SET endpoint=excluded.endpoint,token=excluded.token,"
                "verdict=excluded.verdict,checks_json=excluded.checks_json,checked_at=excluded.checked_at,"
                "last_error=excluded.last_error,updated_at=excluded.updated_at,"
                "sample_json=CASE WHEN excluded.sample_json='{}' THEN app_data_sources.sample_json "
                "ELSE excluded.sample_json END",
                (app_id, endpoint, token, json.dumps(sample or {}, ensure_ascii=False),
                 verdict, json.dumps(checks or [], ensure_ascii=False), ts, last_error, ts),
            )

    def get_app_data_source(self, app_id: str) -> dict[str, Any] | None:
        with self.connect() as db:
            row = db.execute(
                "SELECT app_id,endpoint,token,sample_json,verdict,checks_json,checked_at,last_error,updated_at "
                "FROM app_data_sources WHERE app_id=?", (app_id,),
            ).fetchone()
        if not row:
            return None
        result = dict(row)
        try:
            sample = json.loads(result.get("sample_json") or "{}")
        except json.JSONDecodeError:
            sample = {}
        try:
            checks = json.loads(result.get("checks_json") or "[]")
        except json.JSONDecodeError:
            checks = []
        result["sample"] = sample
        result["checks"] = checks
        return result

    def update_data_source_sample(self, app_id: str, sample: dict[str, Any]) -> None:
        ts = now_iso()
        with self.connect() as db:
            db.execute("UPDATE app_data_sources SET sample_json=?,checked_at=?,updated_at=? WHERE app_id=?",
                       (json.dumps(sample or {}, ensure_ascii=False), ts, ts, app_id))

    def delete_app_data_source(self, app_id: str) -> None:
        with self.connect() as db:
            db.execute("DELETE FROM app_data_sources WHERE app_id=?", (app_id,))

    def submit_sandbox_app(self, app_id: str, developer_id: str, version: str, notes: str) -> bool:
        with self.connect() as db:
            cur = db.execute("UPDATE sandbox_apps SET status='in_review',version=?,review_note=?,updated_at=? "
                             "WHERE app_id=? AND developer_id=? AND status IN ('sandbox','rejected')",
                             (version, notes, now_iso(), app_id, developer_id))
        return cur.rowcount > 0

    def review_sandbox_app(self, app_id: str, decision: str, note: str) -> bool:
        status_value = "published" if decision == "approve" else "rejected"
        with self.connect() as db:
            cur = db.execute("UPDATE sandbox_apps SET status=?,review_note=?,updated_at=? WHERE app_id=? AND status='in_review'",
                             (status_value, note, now_iso(), app_id))
            changed = cur.rowcount > 0
        if changed:
            if decision == "approve":
                self.publish_app_catalog(app_id, note)
            else:
                self.set_published_app_enabled(app_id, 0)
        return changed

    # —— 已发布应用目录（审核通过的应用在此登记，供内容后台选择并配置到屏幕）——
    def publish_app_catalog(self, app_id: str, review_note: str = "") -> None:
        """审核通过时把应用登记到 published_apps 目录（enabled=1，可下线）。"""
        app = self.sandbox_app_raw(app_id)
        if not app:
            return
        build = self.get_sandbox_build(app_id) or {}
        with self.connect() as db:
            db.execute(
                "INSERT INTO published_apps(app_id,name,description,scene,preview_url,entry,developer_id,enabled,review_note,published_at) "
                "VALUES(?,?,?,?,?,?,?,1,?,?) "
                "ON CONFLICT(app_id) DO UPDATE SET name=excluded.name,description=excluded.description,"
                "scene=excluded.scene,preview_url=excluded.preview_url,entry=excluded.entry,"
                "developer_id=excluded.developer_id,enabled=1,review_note=excluded.review_note,published_at=excluded.published_at",
                (app_id, app["name"], app.get("description") or "", app["scene"],
                 f"/sandbox/{app_id}/", build.get("entry") or "index.html",
                 app.get("developer_id") or "", review_note or "", now_iso()),
            )

    def sandbox_app_raw(self, app_id: str) -> dict[str, Any] | None:
        with self.connect() as db:
            row = db.execute(
                "SELECT app_id,developer_id,name,scene,description,status,version,review_note,created_at,updated_at "
                "FROM sandbox_apps WHERE app_id=?", (app_id,),
            ).fetchone()
        return dict(row) if row else None

    def list_published_apps(self, enabled_only: bool = False) -> list[dict[str, Any]]:
        sql = "SELECT * FROM published_apps" + (" WHERE enabled=1" if enabled_only else "") + " ORDER BY published_at DESC"
        with self.connect() as db:
            return [dict(row) for row in db.execute(sql).fetchall()]

    def get_published_app(self, app_id: str) -> dict[str, Any] | None:
        with self.connect() as db:
            row = db.execute("SELECT * FROM published_apps WHERE app_id=?", (app_id,)).fetchone()
        return dict(row) if row else None

    def set_published_app_enabled(self, app_id: str, enabled: int) -> bool:
        with self.connect() as db:
            cur = db.execute("UPDATE published_apps SET enabled=? WHERE app_id=?", (1 if enabled else 0, app_id))
        return cur.rowcount > 0

    # —— 用户自定义场景（场景中心「自定义场景」持久化，按用户隔离）——
    def create_user_scene(self, user_id: str, scene_key: str, name: str, eyebrow: str = "", desc: str = "", icon: str = "") -> None:
        with self.connect() as db:
            db.execute(
                "INSERT INTO user_scenes(user_id,scene_key,name,eyebrow,desc,icon,created_at,updated_at) "
                "VALUES(?,?,?,?,?,?,?,?) "
                "ON CONFLICT(user_id,scene_key) DO UPDATE SET "
                "name=excluded.name,eyebrow=excluded.eyebrow,desc=excluded.desc,icon=excluded.icon,updated_at=excluded.updated_at",
                (user_id, scene_key, name, eyebrow, desc, icon, now_iso(), now_iso()),
            )

    def list_user_scenes(self, user_id: str) -> list[dict[str, Any]]:
        with self.connect() as db:
            return [dict(r) for r in db.execute(
                "SELECT user_id,scene_key,name,eyebrow,desc,icon,created_at,updated_at "
                "FROM user_scenes WHERE user_id=? ORDER BY created_at ASC",
                (user_id,),
            ).fetchall()]

    def get_user_scene(self, user_id: str, scene_key: str) -> dict[str, Any] | None:
        with self.connect() as db:
            row = db.execute(
                "SELECT user_id,scene_key,name,eyebrow,desc,icon,created_at,updated_at "
                "FROM user_scenes WHERE user_id=? AND scene_key=?",
                (user_id, scene_key),
            ).fetchone()
        return dict(row) if row else None

    def update_user_scene(self, user_id: str, scene_key: str, fields: dict[str, Any]) -> bool:
        allowed = {"name", "eyebrow", "desc", "icon"}
        sets = {k: v for k, v in (fields or {}).items() if k in allowed}
        if not sets:
            return False
        cols = ", ".join(f"{k}=?" for k in sets)
        with self.connect() as db:
            db.execute(
                f"UPDATE user_scenes SET {cols}, updated_at=? WHERE user_id=? AND scene_key=?",
                (*sets.values(), now_iso(), user_id, scene_key),
            )
        return True

    def delete_user_scene(self, user_id: str, scene_key: str) -> bool:
        with self.connect() as db:
            cur = db.execute("DELETE FROM user_scenes WHERE user_id=? AND scene_key=?", (user_id, scene_key))
        return cur.rowcount > 0

    def save_model_config(self, app_id: str, provider: str, model: str, base_url: str,
                          credential_encrypted: str, system_prompt: str) -> None:
        with self.connect() as db:
            existing = db.execute("SELECT credential_encrypted FROM developer_model_configs WHERE app_id=?", (app_id,)).fetchone()
            credential = credential_encrypted or (existing["credential_encrypted"] if existing else "")
            db.execute("INSERT INTO developer_model_configs VALUES(?,?,?,?,?,?,?) ON CONFLICT(app_id) DO UPDATE SET "
                       "provider=excluded.provider,model=excluded.model,base_url=excluded.base_url,"
                       "credential_encrypted=excluded.credential_encrypted,system_prompt=excluded.system_prompt,updated_at=excluded.updated_at",
                       (app_id, provider, model, base_url, credential, system_prompt, now_iso()))

    def get_model_config(self, app_id: str) -> dict[str, Any] | None:
        with self.connect() as db:
            row = db.execute("SELECT provider,model,base_url,credential_encrypted,system_prompt,updated_at "
                             "FROM developer_model_configs WHERE app_id=?", (app_id,)).fetchone()
        if not row:
            return None
        result = dict(row)
        result["api_key_configured"] = bool(result.pop("credential_encrypted"))
        return result

    def authenticate_app(self, token: str) -> dict[str, Any] | None:
        with self.connect() as db:
            row = db.execute("SELECT app_id,name,scopes,status FROM developer_apps WHERE token_hash=?",
                             (self.hash_token(token),)).fetchone()
        if not row or row["status"] != "active":
            return None
        return {**dict(row), "scopes": json.loads(row["scopes"])}

    def app_public(self, app_id: str) -> dict[str, Any] | None:
        """确权页展示用：应用的公开信息（不返回 token_hash）。"""
        with self.connect() as db:
            row = db.execute("SELECT app_id,name,scopes,status,created_at FROM developer_apps WHERE app_id=?",
                             (app_id,)).fetchone()
        if not row:
            return None
        return {**dict(row), "scopes": json.loads(row["scopes"] or "[]")}

    DEFAULT_APP_SCOPES = ["device.content.write", "device.read"]

    def issue_app_credential(self, app_id: str, name: str,
                             scopes: list[str] | None = None) -> dict[str, Any]:
        """为沙箱应用签发/轮换 OpenAPI 凭证，复用同一 app_id 打通「内容应用」与「API 应用」。

        轮换后旧 Token 立即失效（token_hash 被覆盖），调用方须提示开发者更新本地配置。
        """
        granted = list(scopes or self.DEFAULT_APP_SCOPES)
        token = "xw_live_" + secrets.token_urlsafe(32)
        token_hash = self.hash_token(token)
        now = now_iso()
        with self.connect() as db:
            exists = db.execute("SELECT 1 FROM developer_apps WHERE app_id=?", (app_id,)).fetchone()
            if exists:
                db.execute("UPDATE developer_apps SET name=?,token_hash=?,scopes=?,status='active' WHERE app_id=?",
                           (name, token_hash, json.dumps(granted), app_id))
            else:
                db.execute("INSERT INTO developer_apps(app_id,name,token_hash,scopes,status,created_at) "
                           "VALUES(?,?,?,?,'active',?)", (app_id, name, token_hash, json.dumps(granted), now))
        return {
            "app_id": app_id,
            "app_token": token,
            "scopes": granted,
            "issued_at": now,
            "note": "Token 仅在此刻显示一次，请立即写入应用配置；重新签发会使旧 Token 立即失效。",
        }

    def list_developer_apps(self) -> list[dict[str, Any]]:
        with self.connect() as db:
            rows = db.execute("SELECT app_id,name,scopes,status,created_at FROM developer_apps ORDER BY created_at DESC").fetchall()
        return [{**dict(row), "scopes": json.loads(row["scopes"])} for row in rows]
    def create_webhook(self, app_id: str, url: str, events: list[str]) -> tuple[str, str]:
        webhook_id, secret = "wh-" + secrets.token_hex(6), secrets.token_urlsafe(32)
        with self.connect() as db:
            db.execute("INSERT INTO webhooks VALUES(?,?,?,?,?,?,?)",
                       (webhook_id, app_id, url, json.dumps(events), secret, 1, now_iso()))
        return webhook_id, secret

    def list_webhooks(self, app_id: str) -> list[dict[str, Any]]:
        with self.connect() as db:
            rows = db.execute("SELECT id,url,events,enabled,created_at FROM webhooks WHERE app_id=? ORDER BY created_at DESC",
                              (app_id,)).fetchall()
        return [{**dict(row), "events": json.loads(row["events"]), "enabled": bool(row["enabled"])} for row in rows]

    def record_device_event(self, device_id: str, event_type: str, data: dict[str, Any], occurred_at: str | None) -> str:
        event_id, received = "evt-" + secrets.token_hex(8), now_iso()
        payload = {"id": event_id, "device_id": device_id, "type": event_type,
                   "data": data, "occurred_at": occurred_at or received, "received_at": received}
        with self.connect() as db:
            db.execute("INSERT INTO device_events VALUES(?,?,?,?,?,?)",
                       (event_id, device_id, event_type, json.dumps(data, ensure_ascii=False), payload["occurred_at"], received))
            rows = db.execute("SELECT id,events FROM webhooks WHERE enabled=1").fetchall()
            for row in rows:
                if "device.event" in json.loads(row["events"]):
                    db.execute("INSERT INTO webhook_outbox VALUES(?,?,?,?,?,?,?,NULL)",
                               ("out-" + secrets.token_hex(8), row["id"], "device.event",
                                json.dumps(payload, ensure_ascii=False), "pending", 0, received))
        return event_id

    def list_device_events(self, device_id: str, limit: int = 100) -> list[dict[str, Any]]:
        with self.connect() as db:
            rows = db.execute("SELECT * FROM device_events WHERE device_id=? ORDER BY received_at DESC LIMIT ?",
                              (device_id, limit)).fetchall()
        return [{**dict(row), "data": json.loads(row["data"])} for row in rows]

    def webhook_outbox(self, limit: int = 100) -> list[dict[str, Any]]:
        with self.connect() as db:
            rows = db.execute("SELECT id,webhook_id,event_type,status,attempts,created_at,delivered_at FROM webhook_outbox ORDER BY created_at DESC LIMIT ?",
                              (limit,)).fetchall()
        return [dict(row) for row in rows]

    # ——— 设备令牌（固件领令牌 + 校验）———
    def mint_device_token(self, device_id: str) -> str:
        token = secrets.token_urlsafe(32)
        with self.connect() as db:
            db.execute("INSERT OR REPLACE INTO device_tokens(device_id,token_hash,created_at) VALUES(?,?,?)",
                       (device_id, self.hash_token(token), now_iso()))
        return token

    def verify_device_token(self, device_id: str, token: str) -> bool:
        if not token:
            return False
        with self.connect() as db:
            for cand in _mac_candidates(device_id):
                row = db.execute(
                    "SELECT token_hash FROM device_tokens WHERE device_id=?", (cand,)
                ).fetchone()
                if row and self.hash_token(token) == row["token_hash"]:
                    return True
        return False

    def unbind_device_self(self, device_id: str) -> None:
        with self.connect() as db:
            db.execute("DELETE FROM device_bindings WHERE device_id=?", (device_id,))
            db.execute("DELETE FROM device_tokens WHERE device_id=?", (device_id,))

    def report_runtime(self, device_id: str, mode: str, awake: bool) -> None:
        with self.connect() as db:
            db.execute("UPDATE devices SET runtime_mode=?, last_seen_at=? WHERE device_id=?",
                       (mode, now_iso(), device_id))

    def report_storage(self, device_id: str, snapshot: dict[str, Any]) -> None:
        """设备上报 TF 卡存储快照（状态 + 各分类文件清单），供后台「TF 卡」面板读取。"""
        with self.connect() as db:
            db.execute("UPDATE devices SET storage_json=?, last_seen_at=? WHERE device_id=?",
                       (json.dumps(snapshot, ensure_ascii=False), now_iso(), device_id))

    def get_storage(self, device_id: str) -> dict[str, Any] | None:
        with self.connect() as db:
            row = db.execute("SELECT storage_json FROM devices WHERE device_id=?", (device_id,)).fetchone()
        if not row or not row["storage_json"]:
            return None
        try:
            return json.loads(row["storage_json"])
        except (json.JSONDecodeError, TypeError):
            return None

    def get_teacher_code(self, device_id: str, auth_code: str) -> dict[str, Any] | None:
        with self.connect() as db:
            row = db.execute("SELECT * FROM teacher_codes WHERE device_id=? AND auth_code=?",
                             (device_id, auth_code.strip().upper())).fetchone()
        if not row:
            return None
        return self._teacher_row(dict(row))

    # ——— 留言（文本 / 语音）———
    @staticmethod
    def _message_row(row: dict[str, Any]) -> dict[str, Any]:
        item = dict(row)
        # 已播放（=已读）状态：语音听过、文本读过都算
        item["played"] = bool(item.get("played_at"))
        item["duration"] = round(float(item["duration"] or 0), 1)
        if item["audio_id"]:
            item["audio_url"] = f"/api/messages/{item['id']}/audio"
        else:
            item["audio_url"] = ""
        return item

    def create_message(self, device_id: str, sender_name: str, kind: str, text: str = "",
                       audio_id: str = "", audio_mime: str = "", duration: float = 0,
                       sender_type: str = "member", sender_id: str | None = None,
                       source: str = "console") -> dict[str, Any]:
        msg_id = secrets.token_hex(10)
        created = now_iso()
        with self.connect() as db:
            db.execute(
                "INSERT INTO messages(id,device_id,sender_name,sender_type,sender_id,kind,text,"
                "audio_id,audio_mime,duration,played_at,created_at,source) VALUES(?,?,?,?,?,?,?,?,?,?,NULL,?,?)",
                (msg_id, device_id, (sender_name or "匿名").strip()[:24], sender_type, sender_id,
                 "voice" if kind == "voice" else "text", (text or "").strip()[:500],
                 audio_id, audio_mime, float(duration or 0), created, source),
            )
            row = db.execute("SELECT * FROM messages WHERE id=?", (msg_id,)).fetchone()
        return self._message_row(dict(row))

    def list_messages(self, device_id: str, limit: int = 50, unread_only: bool = False) -> list[dict[str, Any]]:
        sql = "SELECT * FROM messages WHERE device_id=?"
        if unread_only:
            sql += " AND played_at IS NULL"
        sql += " ORDER BY created_at DESC LIMIT ?"
        with self.connect() as db:
            rows = db.execute(sql, (device_id, limit)).fetchall()
        return [self._message_row(dict(r)) for r in rows]

    def get_message(self, message_id: str) -> dict[str, Any] | None:
        with self.connect() as db:
            row = db.execute("SELECT * FROM messages WHERE id=?", (message_id,)).fetchone()
        return self._message_row(dict(row)) if row else None

    def count_unread_messages(self, device_id: str) -> int:
        with self.connect() as db:
            row = db.execute("SELECT COUNT(*) AS c FROM messages WHERE device_id=? AND played_at IS NULL",
                             (device_id,)).fetchone()
        return int(row["c"] or 0)

    def mark_message_played(self, device_id: str, message_id: str) -> bool:
        with self.connect() as db:
            cur = db.execute("UPDATE messages SET played_at=? WHERE id=? AND device_id=? AND played_at IS NULL",
                             (now_iso(), message_id, device_id))
        return cur.rowcount > 0

    def mark_all_messages_played(self, device_id: str) -> int:
        with self.connect() as db:
            cur = db.execute("UPDATE messages SET played_at=? WHERE device_id=? AND played_at IS NULL",
                             (now_iso(), device_id))
        return cur.rowcount

    def delete_message(self, device_id: str, message_id: str) -> bool:
        with self.connect() as db:
            cur = db.execute("DELETE FROM messages WHERE id=? AND device_id=?", (message_id, device_id))
        return cur.rowcount > 0


    # ——— 用户级连接器绑定（OAuth 凭据入保险库）———
    def save_connector_binding(self, user_id: str, connector_id: str, credential_encrypted: str,
                               external_account: str, scopes: list[str],
                               expires_at: str | None = None) -> str:
        ts = now_iso()
        with self.connect() as db:
            db.execute(
                "INSERT INTO user_connectors(user_id,connector_id,credential_encrypted,external_account,"
                "scopes,status,expires_at,updated_at) VALUES(?,?,?,?,?,'active',?,?) "
                "ON CONFLICT(user_id,connector_id) DO UPDATE SET credential_encrypted=excluded.credential_encrypted,"
                "external_account=excluded.external_account, scopes=excluded.scopes, status='active',"
                "expires_at=excluded.expires_at, updated_at=excluded.updated_at",
                (user_id, connector_id, credential_encrypted, external_account,
                 json.dumps(scopes, ensure_ascii=False), expires_at, ts),
            )
        return ts

    def get_connector_binding(self, user_id: str, connector_id: str) -> dict[str, Any] | None:
        with self.connect() as db:
            row = db.execute("SELECT * FROM user_connectors WHERE user_id=? AND connector_id=?",
                             (user_id, connector_id)).fetchone()
        if not row:
            return None
        item = dict(row)
        item["scopes"] = json.loads(item["scopes"])
        return item

    def list_connector_bindings(self, user_id: str) -> list[dict[str, Any]]:
        with self.connect() as db:
            rows = db.execute("SELECT * FROM user_connectors WHERE user_id=? ORDER BY updated_at DESC",
                              (user_id,)).fetchall()
        return [{**dict(r), "scopes": json.loads(r["scopes"])} for r in rows]

    def delete_connector_binding(self, user_id: str, connector_id: str) -> bool:
        with self.connect() as db:
            cur = db.execute("DELETE FROM user_connectors WHERE user_id=? AND connector_id=?",
                             (user_id, connector_id))
        return cur.rowcount > 0

    def create_connector_state(self, user_id: str, connector_id: str, ttl_seconds: int = 600) -> str:
        state = secrets.token_urlsafe(24)
        expires = (datetime.now(UTC) + timedelta(seconds=ttl_seconds)).isoformat()
        with self.connect() as db:
            db.execute("INSERT INTO connector_auth_states VALUES(?,?,?,?,?)",
                       (state, user_id, connector_id, now_iso(), expires))
        return state

    def consume_connector_state(self, state: str) -> dict[str, Any] | None:
        with self.connect() as db:
            row = db.execute("SELECT * FROM connector_auth_states WHERE state=?", (state,)).fetchone()
            if row:
                db.execute("DELETE FROM connector_auth_states WHERE state=?", (state,))
        if not row or row["expires_at"] < now_iso():
            return None
        return {"user_id": row["user_id"], "connector_id": row["connector_id"]}


DEFAULT_TEMPLATES = [
    {"template_id": "notice-card", "name": "通用通知", "category": "general", "schema_definition": {"required": ["title", "content"]}},
    {"template_id": "homework-list", "name": "今日作业", "category": "education", "schema_definition": {"required": ["title", "items"]}},
    {"template_id": "vocabulary-card", "name": "单词学习", "category": "education", "schema_definition": {"required": ["word", "meaning"]}},
    {"template_id": "care-reminder", "name": "照护提醒", "category": "elder-care", "schema_definition": {"required": ["title", "time", "content"], "notice": "仅作提醒，不构成医疗建议"}},
    {"template_id": "daily-schedule", "name": "今日安排", "category": "elder-care", "schema_definition": {"required": ["title", "items"]}},
]
