# HTTP API

All endpoints: `POST /api/<name>`. Request and response bodies are JSON.
Response is sent with chunked transfer encoding. All responses include a `status` field (`"ok"` or error string).

Timestamps in request/response are **Unix timestamps (seconds)** unless noted otherwise.
Money values (balances, payouts, thresholds) are formatted as **decimal strings** (e.g. `"0.01234567"`).

## Authentication

- **None** — endpoint is public
- **Session** — requires `id` (or `sessionId`) field with a valid session token from `userLogin`
- **Admin** — requires admin session (login with `adminPasswordHash`)
- **Observer** — requires admin or observer session

When `targetLogin` is provided:
- Admin/observer can query any user
- Regular users can only query themselves (targetLogin is ignored or must match own login)

# Table of contents

* [Common status values](#common-status-values)
* [User management](#user-management)
  * [userCreate](#usercreate)
  * [userResendEmail](#userresendemail)
  * [userAction](#useraction)
  * [userLogin](#userlogin)
  * [userLogout](#userlogout)
  * [userQueryMonitoringSession](#userquerymonitoringsession)
  * [userChangePasswordInitiate](#userchangepasswordinitiate)
  * [userChangePasswordForce](#userchangepasswordforce)
  * [userGetCredentials](#usergetcredentials)
  * [userUpdateCredentials](#userupdatecredentials)
  * [userGetSettings](#usergetsettings)
  * [userUpdateSettings](#userupdatesettings)
  * [userEnumerateAll](#userenumerateall)
  * [userActivate2faInitiate](#useractivate2fainitiate)
  * [userDeactivate2faInitiate](#userdeactivate2fainitiate)
  * [userAdjustInstantPayoutThreshold](#useradjustinstantpayoutthreshold)
* [Fee plan management](#fee-plan-management)
  * [userEnumerateFeePlan](#userenumeratefeeplan)
  * [userCreateFeePlan](#usercreatefeeplan)
  * [userQueryFeePlan](#userqueryfeeplan)
  * [userUpdateFeePlan](#userupdatefeeplan)
  * [userDeleteFeePlan](#userdeletefeeplan)
  * [userChangeFeePlan](#userchangefeeplan)
  * [userRenewFeePlanReferralId](#userrenewfeeplanreferralid)
* [Backend queries](#backend-queries)
  * [backendQueryCoins](#backendquerycoins)
  * [backendQueryFoundBlocks](#backendqueryfoundblocks)
  * [backendQueryPoolStats](#backendquerypoolstats)
  * [backendQueryPoolStatsHistory](#backendquerypoolstatshistory)
  * [backendPoolLuck](#backendpoolluck)
  * [instanceEnumerateAll](#instanceenumerateall)
* [User backend queries](#user-backend-queries)
  * [backendQueryUserBalance](#backendqueryuserbalance)
  * [backendQueryUserStats](#backendqueryuserstats)
  * [backendQueryUserStatsHistory](#backendqueryuserstatshistory)
  * [backendQueryWorkerStatsHistory](#backendqueryworkerstatshistory)
  * [backendQueryPayouts](#backendquerypayouts)
  * [backendManualPayout](#backendmanualpayout)
  * [backendQueryPPLNSPayouts](#backendquerypplnspayouts)
  * [backendQueryPPLNSAcc](#backendquerypplnsacc)
  * [backendQueryPPSPayouts](#backendquerippspayouts)
  * [backendQueryPPSPayoutsAcc](#backendquerippspayoutsacc)
* [Admin endpoints](#admin-endpoints)
  * [backendGetConfig](#backendgetconfig)
  * [backendUpdateConfig](#backendupdateconfig)
  * [backendGetPPSState](#backendgetppsstate)
  * [backendQueryPPSHistory](#backendqueryppshistory)
  * [backendQueryProfitSwitchCoeff](#backendqueryprofitswitchcoeff)
  * [backendUpdateProfitSwitchCoeff](#backendupdateprofitswitchcoeff)
  * [complexMiningStatsGetInfo](#complexminingstatsgettinfo)

---

## Common status values

| Status | Description |
|--------|-------------|
| `"ok"` | Operation succeeded |
| `"invalid_json"` | Request body is not valid JSON |
| `"unknown_error"` | Unspecified server error |
| `"unknown_id"` | Invalid session or action ID |
| `"invalid_password"` | Wrong password |
| `"duplicate_email"` | Email already registered |
| `"duplicate_login"` | Login already exists |
| `"user_not_active"` | User account not activated |
| `"2fa_required"` | Two-factor authentication code required |
| `"invalid_2fa"` | Invalid 2FA code |
| `"not_implemented"` | Feature not yet implemented |

---

## User management

### userCreate

Creates a new user account. If SMTP is enabled, sends an activation email.

- **Auth**: none (public registration) or session (admin creating user)

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `id` | string | no | `""` | Session ID (admin can set isActive, feePlanId) |
| `login` | string | yes | | Username |
| `password` | string | yes | | Password |
| `name` | string | no | `""` | Display name |
| `email` | string | no | `""` | Email address |
| `totp` | string | no | `""` | 2FA code (if parent user has 2FA) |
| `isActive` | bool | no | `false` | Activate immediately (admin only) |
| `isReadOnly` | bool | no | `false` | Read-only account |
| `feePlanId` | string | no | `""` | Assign fee plan (admin only) |
| `referralId` | string | no | `""` | Referral ID for fee plan (cannot combine with feePlanId) |

**Response**: `status`

---

### userResendEmail

Resends the activation email for an inactive account.

- **Auth**: none

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `login` | string | yes | | Username |
| `password` | string | yes | | Password |
| `email` | string | no | `""` | Email address |

**Response**: `status`

---

### userAction

Performs a pending user action (account activation, password change, 2FA toggle).

- **Auth**: none (uses action token)

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `actionId` | string | yes | | Action token (from email link) |
| `newPassword` | string | no | `""` | New password (for password change actions) |
| `totp` | string | no | `""` | 2FA code (for 2FA activation) |

**Response**: `status`

---

### userLogin

Authenticates a user and returns a session token.

- **Auth**: none

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `login` | string | yes | | Username |
| `password` | string | yes | | Password |
| `totp` | string | no | `""` | 2FA code (required if 2FA enabled) |

**Response**:

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | |
| `sessionid` | string | Session token for subsequent requests |
| `isReadOnly` | bool | Whether session is read-only |

---

### userLogout

Invalidates a session.

- **Auth**: none (session self-invalidation)

| Request field | Type | Required | Description |
|---------------|------|----------|-------------|
| `id` | string | yes | Session ID to invalidate |

**Response**: `status`

---

### userQueryMonitoringSession

Creates a read-only monitoring session for a user. Admin can create sessions for other users via `targetLogin`.

- **Auth**: session

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `id` | string | yes | | Session ID |
| `targetLogin` | string | no | `""` | Target user (admin only) |

**Response**:

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | |
| `sessionid` | string | Monitoring session token |

---

### userChangePasswordInitiate

Initiates password change by sending a reset email.

- **Auth**: none

| Request field | Type | Required | Description |
|---------------|------|----------|-------------|
| `login` | string | yes | Username |

**Response**: `status`

---

### userChangePasswordForce

Changes password using an action token (from email link).

- **Auth**: none (uses action token)

| Request field | Type | Required | Description |
|---------------|------|----------|-------------|
| `id` | string | yes | Action token |
| `login` | string | yes | Username |
| `newPassword` | string | yes | New password |

**Response**: `status`

---

### userGetCredentials

Returns user profile information.

- **Auth**: session

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `id` | string | yes | | Session ID |
| `targetLogin` | string | no | `""` | Target user (admin only) |

**Response**:

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | |
| `login` | string | Username |
| `name` | string | Display name |
| `email` | string | Email |
| `registrationDate` | int | Unix timestamp |
| `isActive` | bool | Account active |
| `isReadOnly` | bool | Read-only account |
| `has2fa` | bool | 2FA enabled |

---

### userUpdateCredentials

Updates user profile (name, email, password).

- **Auth**: session

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `id` | string | yes | | Session ID |
| `targetLogin` | string | no | `""` | Target user (admin only) |
| `name` | string | no | `""` | New display name |
| `email` | string | no | `""` | New email |
| `password` | string | no | `""` | New password |
| `isActive` | bool | no | `false` | Activate/deactivate (admin only) |
| `isReadOnly` | bool | no | `false` | Set read-only (admin only) |

**Response**: `status`

---

### userGetSettings

Returns per-coin mining and payout settings for a user.

- **Auth**: session

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `id` | string | yes | | Session ID |
| `targetLogin` | string | no | `""` | Target user (admin only) |

**Response**:

```json
{
  "status": "ok",
  "coins": [
    {
      "name": "BTC",
      "payout": { "mode": "enabled", "address": "1abc...", "instantPayoutThreshold": "0.1" },
      "mining": { "mode": "PPLNS" },
      "autoExchange": { "payoutCoinName": "" }
    }
  ]
}
```

---

### userUpdateSettings

Updates per-coin mining and payout settings. Requires TOTP for address changes if 2FA is enabled.

- **Auth**: session

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `id` | string | yes | | Session ID |
| `targetLogin` | string | no | `""` | Target user (admin only) |
| `coin` | string | yes | | Coin name (e.g. "BTC") |
| `totp` | string | no | `""` | 2FA code |
| `payout` | object | no | | `{mode, address, instantPayoutThreshold}` |
| `mining` | object | no | | `{mode}` — "PPLNS" or "PPS" |
| `autoExchange` | object | no | | `{payoutCoinName}` |

At least one of `payout`, `mining`, `autoExchange` must be provided.

**Response**: `status`

---

### userEnumerateAll

Lists all users with statistics. Admin/observer only.

- **Auth**: admin or observer

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `id` | string | yes | | Session ID |
| `coin` | string | no | `"sha256"` | Coin/algorithm for stats |
| `offset` | uint64 | no | `0` | Pagination offset |
| `size` | uint64 | no | `100` | Page size |
| `sortBy` | string | no | `"averagePower"` | Sort field: `login`, `workersNum`, `averagePower`, `sharesPerSecond`, `lastShareTime` |
| `sortDescending` | bool | no | `true` | Sort direction |

**Response**:

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | |
| `users` | array | Array of user objects |

Each user object:

| Field | Type | Description |
|-------|------|-------------|
| `login` | string | Username |
| `name` | string | Display name |
| `email` | string | Email |
| `registrationDate` | int | Unix timestamp |
| `isActive` | bool | |
| `isReadOnly` | bool | |
| `feePlanId` | string | |
| `workers` | int | Active worker count |
| `shareRate` | double | Shares per second |
| `power` | int | Hash rate |
| `lastShareTime` | int | Unix timestamp |

---

### userActivate2faInitiate

Starts 2FA activation. Returns the TOTP secret key for QR code generation.

- **Auth**: session

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `sessionId` | string | yes | | Session ID |
| `targetLogin` | string | no | `""` | Target user (admin only) |

**Response**:

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | |
| `key` | string | TOTP secret key (base32) |

---

### userDeactivate2faInitiate

Starts 2FA deactivation. Sends confirmation email.

- **Auth**: session

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `sessionId` | string | yes | | Session ID |
| `targetLogin` | string | no | `""` | Target user (admin only) |

**Response**: `status`

---

### userAdjustInstantPayoutThreshold

Sets the global instant payout threshold for a coin. Admin only.

- **Auth**: admin

| Request field | Type | Required | Description |
|---------------|------|----------|-------------|
| `id` | string | yes | Session ID |
| `coin` | string | yes | Coin name |
| `threshold` | string | yes | Money value (decimal string) |

**Response**: `status`

---

## Fee plan management

All fee plan endpoints require admin session.

### userEnumerateFeePlan

Lists all fee plan IDs.

- **Auth**: session

| Request field | Type | Required | Description |
|---------------|------|----------|-------------|
| `id` | string | yes | Session ID |

**Response**:

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | |
| `plans` | array | Array of fee plan ID strings |

---

### userCreateFeePlan

Creates a new empty fee plan.

- **Auth**: session

| Request field | Type | Required | Description |
|---------------|------|----------|-------------|
| `id` | string | yes | Session ID |
| `feePlanId` | string | yes | New fee plan ID |

**Response**: `status`

---

### userQueryFeePlan

Returns fee plan configuration for a specific mining mode.

- **Auth**: session

| Request field | Type | Required | Description |
|---------------|------|----------|-------------|
| `id` | string | yes | Session ID |
| `feePlanId` | string | yes | Fee plan ID |
| `mode` | string | yes | `"PPLNS"` or `"PPS"` |

**Response**:

```json
{
  "status": "ok",
  "feePlanId": "default",
  "mode": "PPLNS",
  "referralId": "abc123...",
  "default": [
    { "userId": "pool", "percentage": 1.0 }
  ],
  "coinSpecific": [
    { "coinName": "BTC", "config": [{ "userId": "pool", "percentage": 0.5 }] }
  ]
}
```

---

### userUpdateFeePlan

Updates fee plan configuration for a specific mining mode.

- **Auth**: session

| Request field | Type | Required | Description |
|---------------|------|----------|-------------|
| `id` | string | yes | Session ID |
| `feePlanId` | string | yes | Fee plan ID |
| `mode` | string | yes | `"PPLNS"` or `"PPS"` |
| `default` | array | yes | `[{userId, percentage}]` |
| `coinSpecific` | array | yes | `[{coinName, config: [{userId, percentage}]}]` |

**Response**: `status`

---

### userDeleteFeePlan

Deletes a fee plan.

- **Auth**: session

| Request field | Type | Required | Description |
|---------------|------|----------|-------------|
| `id` | string | yes | Session ID |
| `feePlanId` | string | yes | Fee plan ID to delete |

**Response**: `status`

---

### userChangeFeePlan

Assigns a fee plan to a user.

- **Auth**: session

| Request field | Type | Required | Description |
|---------------|------|----------|-------------|
| `id` | string | yes | Session ID |
| `targetLogin` | string | yes | User to update |
| `feePlanId` | string | yes | Fee plan ID to assign |

**Response**: `status`

---

### userRenewFeePlanReferralId

Regenerates the referral ID for a fee plan.

- **Auth**: session

| Request field | Type | Required | Description |
|---------------|------|----------|-------------|
| `id` | string | yes | Session ID |
| `feePlanId` | string | yes | Fee plan ID |

**Response**:

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | |
| `referralId` | string | New referral ID (may be absent if generation fails) |

---

## Backend queries

Public endpoints for pool information.

### backendQueryCoins

Returns information about all supported coins.

- **Auth**: none (optional session for user-specific fee rates)

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `id` | string | no | `""` | Session ID (for fee calculation) |

**Response**:

```json
{
  "status": "ok",
  "coins": [
    {
      "name": "BTC",
      "fullName": "Bitcoin",
      "algorithm": "sha256",
      "ppsAvailable": true,
      "pplnsFee": 1.0,
      "ppsFee": 2.0,
      "minimalRegularPayout": "0.01",
      "minimalInstantPayout": "0.001",
      "acceptIncoming": false,
      "acceptOutgoing": false,
      "valueBTC": 1.0,
      "valueUSD": 60000.0
    }
  ]
}
```

---

### backendQueryFoundBlocks

Returns blocks found by the pool, paginated by height.

- **Auth**: none

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `coin` | string | yes | | Coin name |
| `heightFrom` | int64 | no | `-1` | Start height (-1 = latest) |
| `hashFrom` | string | no | `""` | Start hash (for pagination) |
| `count` | uint | no | `20` | Number of blocks to return |

**Response**:

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | |
| `blocks` | array | Array of block objects |

Each block:

| Field | Type | Description |
|-------|------|-------------|
| `height` | int | Block height |
| `hash` | string | Block hash |
| `time` | int | Unix timestamp |
| `confirmations` | int | Current confirmations (-1 = orphan) |
| `generatedCoins` | string | Block reward (decimal string) |
| `foundBy` | string | User who found the block |
| `mergedBlocks` | array | `[{coin, height, hash}]` (merged mining) |

---

### backendQueryPoolStats

Returns current pool-wide statistics.

- **Auth**: none

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `coin` | string | no | `""` | Coin name (empty = all coins) |

**Response**:

```json
{
  "status": "ok",
  "currentTime": 1700000000,
  "stats": [
    {
      "coin": "BTC",
      "powerUnit": "Gh/s",
      "powerMultLog10": 9,
      "clients": 42,
      "workers": 100,
      "shareRate": 15.5,
      "shareWork": "12345678",
      "power": 1500,
      "lastShareTime": 1700000000
    }
  ]
}
```

---

### backendQueryPoolStatsHistory

Returns pool statistics history.

- **Auth**: none

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `coin` | string | no | `""` | Coin name |
| `timeFrom` | int64 | no | now - 86400 | Start time (Unix) |
| `timeTo` | int64 | no | now | End time (Unix) |
| `groupByInterval` | int64 | no | `3600` | Aggregation interval (seconds) |

**Response**:

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | |
| `powerUnit` | string | e.g. "Gh/s" |
| `powerMultLog10` | int | Power multiplier log10 |
| `currentTime` | int | Server time |
| `stats` | array | `[{name, time, shareRate, shareWork, power}]` |

---

### backendPoolLuck

Calculates pool luck for specified time intervals.

- **Auth**: none

| Request field | Type | Required | Description |
|---------------|------|----------|-------------|
| `coin` | string | yes | Coin name |
| `intervals` | array | yes | Array of Unix timestamps (must be strictly increasing) |

**Response**:

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | |
| `luck` | array | Array of doubles (luck values per interval) |

---

### instanceEnumerateAll

Lists all mining protocol instances.

- **Auth**: none

**Response**:

```json
{
  "status": "ok",
  "instances": [
    {
      "protocol": "stratum",
      "type": "BTC",
      "port": 3456,
      "backends": ["BTC"],
      "shareDiff": 65536
    },
    {
      "protocol": "zmq",
      "type": "XPM",
      "port": 6666,
      "backends": ["XPM"]
    }
  ]
}
```

`shareDiff` is only present for stratum instances.

---

## User backend queries

Endpoints requiring user session for per-user data.

### backendQueryUserBalance

Returns user balance across one or all coins.

- **Auth**: session

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `id` | string | yes | | Session ID |
| `targetLogin` | string | no | `""` | Target user (admin only) |
| `coin` | string | no | `""` | Coin name (empty = all coins) |

**Response**:

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | |
| `balances` | array | `[{coin, balance, requested, paid, queued}]` — all money as decimal strings |

---

### backendQueryUserStats

Returns current worker statistics for a user.

- **Auth**: session

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `id` | string | yes | | Session ID |
| `targetLogin` | string | no | `""` | Target user |
| `coin` | string | no | `""` | Coin name |
| `offset` | uint64 | no | `0` | Pagination offset |
| `size` | uint64 | no | `4096` | Page size |
| `sortBy` | string | no | `"name"` | Sort: `name`, `averagePower`, `sharesPerSecond`, `lastShareTime` |
| `sortDescending` | bool | no | `false` | Sort direction |

**Response**:

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | |
| `powerUnit` | string | e.g. "Gh/s" |
| `powerMultLog10` | int | |
| `currentTime` | int | Server time |
| `total` | object | Aggregate: `{clients, workers, shareRate, shareWork, power, lastShareTime}` |
| `workers` | array | `[{name, shareRate, shareWork, power, lastShareTime}]` |

---

### backendQueryUserStatsHistory

Returns historical statistics for a user.

- **Auth**: session

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `id` | string | yes | | Session ID |
| `targetLogin` | string | no | `""` | Target user |
| `coin` | string | no | `""` | Coin name |
| `timeFrom` | int64 | no | now - 86400 | Start time (Unix) |
| `timeTo` | int64 | no | now | End time (Unix) |
| `groupByInterval` | int64 | no | `3600` | Aggregation interval (seconds) |

**Response**:

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | |
| `powerUnit` | string | |
| `powerMultLog10` | int | |
| `currentTime` | int | |
| `stats` | array | `[{name, time, shareRate, shareWork, power}]` |

---

### backendQueryWorkerStatsHistory

Returns historical statistics for a specific worker.

- **Auth**: session

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `id` | string | yes | | Session ID |
| `targetLogin` | string | no | `""` | Target user |
| `coin` | string | yes | | Coin name |
| `workerId` | string | yes | | Worker name |
| `timeFrom` | int64 | no | now - 86400 | Start time (Unix) |
| `timeTo` | int64 | no | now | End time (Unix) |
| `groupByInterval` | int64 | no | `3600` | Aggregation interval (seconds) |

**Response**: same format as `backendQueryUserStatsHistory`

---

### backendQueryPayouts

Returns payout transaction history for a user.

- **Auth**: session

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `id` | string | yes | | Session ID |
| `targetLogin` | string | no | `""` | Target user |
| `coin` | string | yes | | Coin name |
| `timeFrom` | int64 | no | `0` | Start time (Unix) |
| `count` | uint | no | `20` | Number of payouts |

**Response**:

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | |
| `payouts` | array | Payout objects |

Each payout:

| Field | Type | Description |
|-------|------|-------------|
| `time` | int | Unix timestamp |
| `txid` | string | Transaction ID |
| `value` | string | Amount (decimal string) |
| `valueBTC` | string | BTC equivalent |
| `valueUSD` | string | USD equivalent |
| `status` | int | 0=Initialized, 1=TxCreated, 2=TxSent, 3=TxConfirmed, 4=TxRejected |

---

### backendManualPayout

Triggers an immediate payout for a user.

- **Auth**: session (write access required)

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `id` | string | yes | | Session ID |
| `targetLogin` | string | no | `""` | Target user (admin only) |
| `coin` | string | yes | | Coin name |

**Response**: `status`

---

### backendQueryPPLNSPayouts

Returns PPLNS payout records per round.

- **Auth**: session

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `id` | string | yes | | Session ID |
| `targetLogin` | string | no | `""` | Target user |
| `coin` | string | yes | | Coin name |
| `timeFrom` | int64 | no | `0` | Start time (Unix) |
| `hashFrom` | string | no | `""` | Start hash (pagination) |
| `count` | uint | no | `20` | Number of records |

**Response**:

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | |
| `payouts` | array | `[{startTime, endTime, hash, height, value, valueBTC, valueUSD}]` |

---

### backendQueryPPLNSAcc

Returns accumulated PPLNS payouts grouped by time intervals.

- **Auth**: session

| Request field | Type | Required | Description |
|---------------|------|----------|-------------|
| `id` | string | yes | Session ID |
| `targetLogin` | string | no | Target user |
| `coin` | string | yes | Coin name |
| `timeFrom` | int64 | yes | Start time (Unix) |
| `timeTo` | int64 | yes | End time (Unix) |
| `groupByInterval` | int64 | yes | Aggregation interval (seconds) |

Constraint: `(timeTo - timeFrom)` must be divisible by `groupByInterval`. Max 3200 intervals.

**Response**:

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | |
| `accumulated` | array | `[{time, accumulated}]` — accumulated as decimal string |

---

### backendQueryPPSPayouts

Returns PPS payout records.

- **Auth**: session

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `id` | string | yes | | Session ID |
| `targetLogin` | string | no | `""` | Target user |
| `coin` | string | yes | | Coin name |
| `timeFrom` | int64 | no | `0` | Start time (Unix) |
| `count` | uint | no | `20` | Number of records |

**Response**:

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | |
| `payouts` | array | `[{startTime, endTime, value, valueBTC, valueUSD}]` |

---

### backendQueryPPSPayoutsAcc

Returns accumulated PPS payouts grouped by time intervals.

- **Auth**: session

| Request field | Type | Required | Description |
|---------------|------|----------|-------------|
| `id` | string | yes | Session ID |
| `targetLogin` | string | no | Target user |
| `coin` | string | yes | Coin name |
| `timeFrom` | int64 | yes | Start time (Unix) |
| `timeTo` | int64 | yes | End time (Unix) |
| `groupByInterval` | int64 | yes | Aggregation interval (seconds) |

Constraint: `(timeTo - timeFrom)` must be divisible by `groupByInterval`. Max 3200 intervals.

**Response**: same format as `backendQueryPPLNSAcc`

---

## Admin endpoints

### backendGetConfig

Returns backend configuration for a coin.

- **Auth**: admin or observer

| Request field | Type | Required | Description |
|---------------|------|----------|-------------|
| `id` | string | yes | Session ID |
| `coin` | string | yes | Coin name |

**Response**:

```json
{
  "status": "ok",
  "pps": {
    "enabled": false,
    "poolFee": 2.0,
    "saturationFunction": "None",
    "saturationB0": 0.0,
    "saturationANegative": 0.0,
    "saturationAPositive": 0.0,
    "saturationFunctions": ["None", "Tangent", "Clamp", "Cubic", "Softsign", "Norm", "Atan", "Exp"]
  },
  "payouts": {
    "instantPayoutsEnabled": true,
    "regularPayoutsEnabled": true,
    "instantMinimalPayout": "0.001",
    "instantPayoutInterval": 3600000,
    "regularMinimalPayout": "0.01",
    "regularPayoutInterval": 86400000,
    "regularPayoutDayOffset": 0
  },
  "swap": {
    "acceptIncoming": false,
    "acceptOutgoing": false
  }
}
```

Note: payout intervals are in milliseconds.

---

### backendUpdateConfig

Updates backend configuration for a coin.

- **Auth**: admin

| Request field | Type | Required | Description |
|---------------|------|----------|-------------|
| `id` | string | yes | Session ID |
| `coin` | string | yes | Coin name |
| `pps` | object | no | PPS settings (all fields optional) |
| `payouts` | object | no | Payout settings (all fields optional) |
| `swap` | object | no | Swap settings (all fields optional) |

At least one of `pps`, `payouts`, `swap` must be present.

**pps** fields: `enabled`(bool), `poolFee`(double), `saturationFunction`(string), `saturationB0`(double), `saturationANegative`(double), `saturationAPositive`(double)

**payouts** fields: `instantPayoutsEnabled`(bool), `regularPayoutsEnabled`(bool), `instantMinimalPayout`(string), `instantPayoutInterval`(int64 ms), `regularMinimalPayout`(string), `regularPayoutInterval`(int64 ms), `regularPayoutDayOffset`(int64 ms, default 0)

**swap** fields: `acceptIncoming`(bool), `acceptOutgoing`(bool)

**Response**: `status`

---

### backendGetPPSState

Returns the current PPS pool state.

- **Auth**: admin or observer

| Request field | Type | Required | Description |
|---------------|------|----------|-------------|
| `id` | string | yes | Session ID |
| `coin` | string | yes | Coin name |

**Response**:

```json
{
  "status": "ok",
  "ppsState": {
    "time": 1700000000,
    "balance": { "value": "1.23456789", "inBlocks": 0.5 },
    "refBalance": { "value": "2.00000000", "inBlocks": 0.8, "sqLambda": 0.1 },
    "minRefBalance": { "time": 1699900000, "value": "1.5", "inBlocks": 0.6, "sqLambda": 0.05 },
    "maxRefBalance": { "time": 1699950000, "value": "2.5", "inBlocks": 1.0, "sqLambda": 0.15 },
    "totalBlocksFound": 10.5,
    "orphanBlocks": 0.2,
    "lastSaturateCoeff": 1.0,
    "lastBaseBlockReward": "6.25000000",
    "lastAverageTxFee": "0.00123456"
  }
}
```

---

### backendQueryPPSHistory

Returns historical PPS pool state snapshots.

- **Auth**: admin or observer

| Request field | Type | Required | Description |
|---------------|------|----------|-------------|
| `id` | string | yes | Session ID |
| `coin` | string | yes | Coin name |
| `timeFrom` | int64 | yes | Start time (Unix) |
| `timeTo` | int64 | yes | End time (Unix) |

**Response**:

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | |
| `history` | array | Array of `ppsState` objects (same format as `backendGetPPSState`) |

---

### backendQueryProfitSwitchCoeff

Returns profit switching coefficients for all coins.

- **Auth**: admin or observer

| Request field | Type | Required | Description |
|---------------|------|----------|-------------|
| `id` | string | yes | Session ID |

**Response**:

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | |
| `coins` | array | `[{name, profitSwitchCoeff}]` |

---

### backendUpdateProfitSwitchCoeff

Updates the profit switching coefficient for a coin.

- **Auth**: admin

| Request field | Type | Required | Default | Description |
|---------------|------|----------|---------|-------------|
| `id` | string | yes | | Session ID |
| `coin` | string | no | `""` | Coin name |
| `profitSwitchCoeff` | double | yes | | New coefficient |

**Response**: `status`

---

### complexMiningStatsGetInfo

Queries complex mining statistics engine.

- **Auth**: session

| Request field | Type | Required | Description |
|---------------|------|----------|-------------|
| `id` | string | yes | Session ID |

**Response**: varies by implementation

---

## Endpoint summary

| # | Endpoint | Auth |
|---|----------|------|
| 1 | userCreate | none/session |
| 2 | userResendEmail | none |
| 3 | userAction | none |
| 4 | userLogin | none |
| 5 | userLogout | none |
| 6 | userQueryMonitoringSession | session |
| 7 | userChangeEmail | — (not implemented) |
| 8 | userChangePasswordInitiate | none |
| 9 | userChangePasswordForce | none |
| 10 | userGetCredentials | session |
| 11 | userUpdateCredentials | session |
| 12 | userGetSettings | session |
| 13 | userUpdateSettings | session |
| 14 | userEnumerateAll | admin/observer |
| 15 | userEnumerateFeePlan | session |
| 16 | userCreateFeePlan | session |
| 17 | userQueryFeePlan | session |
| 18 | userUpdateFeePlan | session |
| 19 | userDeleteFeePlan | session |
| 20 | userChangeFeePlan | session |
| 21 | userRenewFeePlanReferralId | session |
| 22 | userActivate2faInitiate | session |
| 23 | userDeactivate2faInitiate | session |
| 24 | userAdjustInstantPayoutThreshold | admin |
| 25 | backendManualPayout | session (write) |
| 26 | backendQueryCoins | none |
| 27 | backendQueryFoundBlocks | none |
| 28 | backendQueryPayouts | session |
| 29 | backendQueryPoolBalance | — (not implemented) |
| 30 | backendQueryPoolStats | none |
| 31 | backendQueryPoolStatsHistory | none |
| 32 | backendQueryProfitSwitchCoeff | admin/observer |
| 33 | backendQueryUserBalance | session |
| 34 | backendQueryUserStats | session |
| 35 | backendQueryUserStatsHistory | session |
| 36 | backendQueryWorkerStatsHistory | session |
| 37 | backendQueryPPLNSPayouts | session |
| 38 | backendQueryPPLNSAcc | session |
| 39 | backendQueryPPSPayouts | session |
| 40 | backendQueryPPSPayoutsAcc | session |
| 41 | backendUpdateProfitSwitchCoeff | admin |
| 42 | backendGetConfig | admin/observer |
| 43 | backendGetPPSState | admin/observer |
| 44 | backendQueryPPSHistory | admin/observer |
| 45 | backendUpdateConfig | admin |
| 46 | backendPoolLuck | none |
| 47 | instanceEnumerateAll | none |
| 48 | complexMiningStatsGetInfo | session |
