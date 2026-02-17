# Database Schema

All databases use RocksDB (via `kvdb<rocksdbBase>`) unless noted otherwise.
Values are little-endian, keys use big-endian for sortable fields (Height, Time).
Strings encoded as VarSize length + raw bytes.

## Common Types

- **`UInt<384>`** — money type (coins, balances, payouts). 48 bytes. Upper 128 bits — integer part in minimal coin units (satoshi, wei); lower 256 bits — fractional part. Unsigned, but `isNegative()` checks high bit for underflow detection.
- **`UInt<256>`** — accepted work (share difficulty). 32 bytes.
- **Timestamp** — int64, milliseconds since epoch.
- **TimeInterval** — pair of Timestamps (begin, end).
- **VarSize** — Bitcoin-style varint: <0xFD = 1 byte; 0xFD..0xFFFF = 3 bytes; 0x10000..0xFFFFFFFF = 5 bytes; larger = 9 bytes.
- **`BaseBlob<N>`** — raw binary blob, N/8 bytes.

---

## Accounting

Owner: `AccountingDb` (`src/poolcore/accounting.cpp`)

### 1. rounds.3

Mining rounds (one per found block).

- **Partition**: "default"
- **Key**: Height (uint64), BlockHash (string)
- **Value**:
  - Version: uint32 = 1
  - Height: uint64
  - BlockHash: string
  - EndTime: Timestamp
  - StartTime: Timestamp
  - TotalShareValue: `UInt<256>`
  - AvailableCoins: `UInt<384>`
  - UserShares: `vector<UserShareValue>`
    - UserId: string
    - ShareValue: `UInt<256>`
    - IncomingWork: `UInt<256>`
  - Payouts: `vector<CUserPayout>`
    - UserId: string
    - Value: `UInt<384>`
    - ValueWithoutFee: `UInt<384>`
    - AcceptedWork: `UInt<256>`
  - FoundBy: string
  - ExpectedWork: `UInt<256>`
  - AccumulatedWork: `UInt<256>`
  - TxFee: `UInt<384>`
  - PrimePOWTarget: uint32

### 2. foundBlocks.2

Blocks found by the pool.

- **Partition**: `height / 1000000`
- **Key**: Height (uint64), Hash (string)
- **Value**:
  - Version: uint32 = 1
  - Height: uint64
  - Hash: string
  - Time: Timestamp
  - AvailableCoins: `UInt<384>`
  - FoundBy: string
  - ExpectedWork: `UInt<256>`
  - AccumulatedWork: `UInt<256>`
  - PublicHash: string

### 3. poolBalance.2

Periodic pool wallet balance snapshots.

- **Partition**: "YYYY.MM"
- **Key**: Time (int64)
- **Value**:
  - Version: uint32 = 1
  - Time: int64
  - Balance: `UInt<384>`
  - Immature: `UInt<384>`
  - Users: `UInt<384>`
  - Queued: `UInt<384>`
  - ConfirmationWait: `UInt<384>`
  - Net: `UInt<384>`

### 4. payouts.2

Individual payout transactions.

- **Partition**: "YYYY.MM" (by Time)
- **Key**: UserId (string), Time (int64), TransactionId (string)
- **Value**:
  - Version: uint32 = 1
  - UserId: string
  - Time: int64
  - Value: `UInt<384>`
  - TransactionId: string
  - TransactionData: string
  - Status: uint32 (0=Initialized, 1=TxCreated, 2=TxSent, 3=TxConfirmed, 4=TxRejected)
  - TxFee: `UInt<384>`

### 5. pplns.payouts.2

Per-user PPLNS payout records per round.

- **Partition**: "YYYY.MM" (by RoundStartTime)
- **Key**: Login (string), RoundStartTime (Timestamp), BlockHash (string)
- **Value**:
  - Version: uint32 = 1
  - Login: string
  - RoundStartTime: Timestamp
  - BlockHash: string
  - BlockHeight: uint64
  - RoundEndTime: Timestamp
  - PayoutValue: `UInt<384>`
  - PayoutValueWithoutFee: `UInt<384>`
  - AcceptedWork: `UInt<256>`
  - PrimePOWTarget: uint32
  - RateToBTC: double
  - RateBTCToUSD: double

### 6. accounting.state

Persistent state for accounting (plain rocksdbBase, not kvdb).

- **Partition**: "default"
- **Records** (string keys):
  - `"lastmsgid"` -> uint64 SavedShareId
  - `"recentstats"` -> `vector<CStatsExportData>` (PPLNS snapshot)
  - `"currentscores"` -> `map<string, UInt<256>>` (per-user work since last block)
  - `"currentroundstart"` -> Timestamp
  - `"payoutqueue"` -> `list<PayoutDbRecord>`

---

## Statistics

Owner: `StatisticDb` (`src/poolcore/statistics.cpp`)

### 7. statistic

Worker/user/pool statistics records, merged via `StatsRecordMergeOperator`.

- **Partition**: "YYYY.MM" (by TimeEnd)
- **Key**: Login (string), WorkerId (string), TimeEnd (Timestamp)
- **Value**:
  - Version: uint32 = 1
  - Login: string
  - WorkerId: string
  - Time: TimeInterval
  - UpdateTime: Timestamp
  - ShareCount: uint64
  - ShareWork: `UInt<256>`
  - PrimePOWTarget: uint32
  - PrimePOWShareCount: `vector<uint64>`

---

## User Management

Owner: `UserManager` (`src/poolcore/usermgr.cpp`)

### 8. users

User accounts.

- **Partition**: "default"
- **Key**: Login (string)
- **Value**:
  - Version: uint32 = 1
  - Login: string
  - EMail: string
  - Name: string
  - ParentUser: string
  - TwoFactorAuthData: string
  - PasswordHash: `BaseBlob<256>`
  - RegistrationDate: int64
  - IsActive: bool
  - IsReadOnly: bool
  - IsSuperUser: bool
  - FeePlanId: string
  - MonitoringSessionId: string

### 9. userfeeplan

Fee plan definitions.

- **Partition**: "default"
- **Key**: FeePlanId (string)
- **Value**:
  - Version: uint32 = 1
  - FeePlanId: string
  - Default: UserFeeConfig (`vector<UserFeePair>`)
    - UserId: string
    - Percentage: double
  - CoinSpecificFee: `vector<CoinSpecificFeeRecord2>`
    - CoinName: string
    - Config: UserFeeConfig

### 10. usersettings.2

Per-user per-coin payout settings.

- **Partition**: "default"
- **Key**: Login (string), Coin (string)
- **Value**:
  - Version: uint32 = 1
  - Login: string
  - Coin: string
  - Address: string
  - MinimalPayout: `UInt<384>`
  - AutoPayout: bool

### 11. useractions

Pending user actions (activation, password reset, 2FA).

- **Partition**: "default"
- **Key**: Id (`BaseBlob<512>`)
- **Value**:
  - Version: uint32 = 1
  - Id: `BaseBlob<512>`
  - Login: string
  - Type: uint32 (0=Activate, 1=ChangePassword, 2=ChangeEmail, 3=TwoFactorActivate, 4=TwoFactorDeactivate)
  - CreationDate: uint64
  - TwoFactorKey: string

### 12. usersessions

Active user sessions.

- **Partition**: "default"
- **Key**: Id (`BaseBlob<512>`)
- **Value**:
  - Version: uint32 = 1
  - Id: `BaseBlob<512>`
  - Login: string
  - LastAccessTime: uint64
  - IsReadOnly: bool
  - IsPermanent: bool

---

## File-based Storage

### ShareLog record format

Common binary record format used by all ShareLog files (`shareLog.h`).

Each record: `ShareLogMsgHeader` + data payload.

- **ShareLogMsgHeader** (16 bytes):
  - Id: uint64 LE — unique message id
  - Length: uint32 LE — payload size in bytes
  - Crc32: uint32 LE — CRC-32C (Castagnoli) of payload bytes
- **Payload**: `Length` bytes, serialized via `DbIo<T>::serialize`
- On replay: CRC-32C is verified; mismatch stops replay at that record
- **File naming**: `{firstMessageId}.dat`
- **File rotation**: new file when current reaches `ShareLogFileSizeLimit` (default 4MB)

### 13. ShareLog (statistic.worklog)

Persistent message log for statistics replay on restart.

- **Path**: `{dbPath}/statistic.worklog/*.dat`
- **Owner**: `StatisticDb`
- **Payload type**: CWorkSummaryBatch
  - CWorkSummaryBatch: TimeInterval + `vector<CWorkSummaryEntry>`

### 14. ShareLog (accounting.worklog)

Persistent message log for accounting replay on restart.

- **Path**: `{dbPath}/accounting.worklog/*.dat`
- **Owner**: `AccountingDb`
- **Payload type**: CUserWorkSummaryBatch
  - CUserWorkSummaryBatch: TimeInterval + `vector<CUserWorkSummary>`
    - CUserWorkSummary: UserId (string), AcceptedWork (`UInt<256>`), SharesNum (uint64), Time (Timestamp), BaseBlockReward (`UInt<384>`), ExpectedWork (`UInt<256>`)

### 15. Statistics Cache (.dat files)

In-memory accumulator state persisted to filesystem.

- **Pool stats**: `{dbPath}/statistic.pool.stats/{gridEndSec}.dat`
- **User stats**: `{dbPath}/statistic.user.stats/{gridEndSec}.dat`
- **Worker stats**: `{dbPath}/statistic.worker.stats/{gridEndSec}.dat`
- **Accounting user stats**: `{dbPath}/accounting.userstats/{gridEndSec}.dat`
- **Format** (CStatsFileData):
  - Version: uint32 = 1
  - LastShareId: uint64
  - Records: `vector<CWorkSummaryEntry>`
    - Version: uint32 = 1
    - UserId: string
    - WorkerId: string
    - Data (CWorkSummary):
      - SharesNum: uint64
      - SharesWork: `UInt<256>`
      - PrimePOWTarget: uint32
      - PrimePOWSharesNum: `vector<uint64>`
