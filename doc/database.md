# Database Schema

All databases use RocksDB (via `kvdb<rocksdbBase>`) unless noted otherwise.
Values are little-endian, keys use big-endian for sortable fields (Height, Time).
Strings encoded as VarSize length + raw bytes.

## Common Types

- **`UInt<384>`** — money type (coins, balances, payouts). 48 bytes. Upper 128 bits — integer part in minimal coin units (satoshi, wei); lower 256 bits — fractional part. Unsigned, but `isNegative()` checks high bit for underflow detection.
- **`UInt<256>`** — accepted work (share difficulty). 32 bytes.
- **`BaseBlob<N>`** — raw binary blob, N/8 bytes.
- **Timestamp** — int64, milliseconds since epoch.
- **TimeInterval** — pair of Timestamps (begin, end).
- **VarSize** — Bitcoin-style varint: <0xFD = 1 byte; 0xFD..0xFFFF = 3 bytes; 0x10000..0xFFFFFFFF = 5 bytes; larger = 9 bytes.

---

## Accounting

Owner: `AccountingDb` (`src/poolcore/accounting.cpp`)

### 1. accounting.rounds

Mining rounds (one per found block).

- **Partition**: "YYYY.MM" (by Block.Time)
- **Key**: `'r'` + Height (uint64), Hash (string)
- **Value**:
  - Version: uint32 = 1
  - Block: `CBlockFoundData`
    - UserId: string
    - Height: uint64
    - Hash: string
    - Time: Timestamp
    - GeneratedCoins: `UInt<384>`
    - ExpectedWork: `UInt<256>`
    - PrimePOWTarget: uint32
    - ShareHash: `BaseBlob<256>`
  - StartTime: Timestamp
  - TotalShareValue: `UInt<256>`
  - AvailableForPPLNS: `UInt<384>`
  - UserShares: `vector<UserShareValue>`
    - UserId: string
    - ShareValue: `UInt<256>`
    - IncomingWork: `UInt<256>`
  - Payouts: `vector<CUserPayout>`
    - UserId: string
    - Value: `UInt<384>`
    - ValueWithoutFee: `UInt<384>`
    - AcceptedWork: `UInt<256>`
  - AccumulatedWork: `UInt<256>`
  - TxFee: `UInt<384>`
  - PPSValue: `UInt<384>`
  - PPSBlockPart: double

### 2. foundBlocks

Blocks found by the pool.

- **Partition**: `height / 1000000`
- **Key**: Height (uint64), Hash (string)
- **Value**:
  - Version: uint32 = 1
  - Height: uint64
  - Hash: string
  - Time: Timestamp
  - GeneratedCoins: `UInt<384>`
  - FoundBy: string
  - ExpectedWork: `UInt<256>`
  - AccumulatedWork: `UInt<256>`
  - PublicHash: string
  - MergedBlocks: `vector<CMergedBlockInfo>`
    - CoinName: string
    - Height: uint64
    - Hash: string
  - PrevFoundHash: string
  - ShareHash: `BaseBlob<256>`

### 3. poolBalance

Periodic pool wallet balance snapshots.

- **Partition**: "YYYY.MM"
- **Key**: Time (Timestamp)
- **Value**:
  - Version: uint32 = 1
  - Time: Timestamp
  - Balance: `UInt<384>`
  - Immature: `UInt<384>`
  - Users: `UInt<384>`
  - Queued: `UInt<384>`
  - ConfirmationWait: `UInt<384>`
  - Net: `UInt<384>`

### 4. payouts

Individual payout transactions.

- **Partition**: "YYYY.MM" (by Time)
- **Key**: UserId (string), Time (Timestamp), TransactionId (string)
- **Value**:
  - Version: uint32 = 1
  - UserId: string
  - Time: Timestamp
  - Value: `UInt<384>`
  - TransactionId: string
  - TransactionData: string
  - Status: uint32 (0=Initialized, 1=TxCreated, 2=TxSent, 3=TxConfirmed, 4=TxRejected)
  - TxFee: `UInt<384>`
  - RateToBTC: double
  - RateBTCToUSD: double

### 5. pplns.payouts

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
  - RateToBTC: double
  - RateBTCToUSD: double

### 6. pps.payouts

Per-user PPS payout records per payout cycle.

- **Partition**: "YYYY.MM" (by IntervalBegin)
- **Key**: Login (string), IntervalBegin (Timestamp)
- **Value**:
  - Version: uint32 = 1
  - Login: string
  - IntervalBegin: Timestamp
  - IntervalEnd: Timestamp
  - PayoutValue: `UInt<384>`
  - RateToBTC: double
  - RateBTCToUSD: double

### 7. pps.history

Historical PPS pool state snapshots.

- **Partition**: "YYYY.MM" (by Time)
- **Key**: Time (Timestamp)
- **Value**:
  - Balance: `UInt<384>`
  - ReferenceBalance: `UInt<384>`
  - LastBaseBlockReward: `UInt<384>`
  - TotalBlocksFound: double
  - OrphanBlocks: double
  - Min: `CPPSBalanceSnapshot` { Balance, TotalBlocksFound, Time }
  - Max: `CPPSBalanceSnapshot` { Balance, TotalBlocksFound, Time }
  - LastSaturateCoeff: double
  - LastAverageTxFee: `UInt<384>`
  - Time: Timestamp

### 8. accounting.state

Persistent state for accounting (plain rocksdbBase, not kvdb).

- **Partition**: "default"
- **Records** (string keys):
  - `".stateid"` -> uint64 StateId
  - `".lastmsgid"` -> uint64 SavedShareId
  - `".recentstats"` -> `vector<CStatsExportData>` (PPLNS snapshot)
  - `".currentscores"` -> `map<string, UInt<256>>` (per-user work since last block)
  - `".ppspending"` -> `unordered_map<string, UInt<384>>` (PPS pending balance by user)
  - `".currentroundstart"` -> Timestamp
  - `".settings"` -> `CBackendSettings` (tagged serialization)
  - `".ppsstate"` -> `CPPSState`
  - `".payoutqueue"` -> `list<PayoutDbRecord>`
  - `"r{Height}{Hash}"` -> MiningRound records
  - `"b{Login}"` -> UserBalanceRecord

---

## Statistics

Owner: `StatisticDb` (`src/poolcore/statistics.cpp`)

### 9. statistic

Worker/user/pool statistics records, merged via `CWorkSummaryMergeOperator`.

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

### 10. users

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
  - RegistrationDate: Timestamp
  - IsActive: bool
  - IsReadOnly: bool
  - IsSuperUser: bool
  - FeePlanId: string
  - MonitoringSessionId: string

### 11. userfeeplan

Fee plan definitions.

- **Partition**: "default"
- **Key**: FeePlanId (string)
- **Value**:
  - FeePlanId: string
  - Modes: `vector<CModeFeeConfig>` (indexed by EMiningMode: 0=PPLNS, 1=PPS)
    - Default: `vector<UserFeePair>`
      - UserId: string
      - Percentage: double
    - CoinSpecific: `vector<CUserFeeConfig>`
      - CoinName: string
      - Config: `vector<UserFeePair>`
  - ReferralId: `BaseBlob<256>`

### 12. usersettings

Per-user per-coin payout and mining settings.

- **Partition**: "default"
- **Key**: Login (string), Coin (string)
- **Value**:
  - Login: string
  - Coin: string
  - Payout: `CSettingsPayout`
    - Mode: uint32 (0=Disabled, 1=Enabled)
    - Address: string
    - InstantPayoutThreshold: `UInt<384>`
  - Mining: `CSettingsMining`
    - MiningMode: uint32 (0=PPLNS, 1=PPS)
  - AutoExchange: `CSettingsAutoExchange`
    - PayoutCoinName: string

### 13. useractions

Pending user actions (activation, password reset, 2FA).

- **Partition**: "default"
- **Key**: Id (`BaseBlob<512>`)
- **Value**:
  - Version: uint32 = 1
  - Id: `BaseBlob<512>`
  - Login: string
  - Type: uint32 (0=Activate, 1=ChangePassword, 2=ChangeEmail, 3=TwoFactorActivate, 4=TwoFactorDeactivate)
  - CreationDate: Timestamp
  - TwoFactorKey: string

### 14. usersessions

Active user sessions.

- **Partition**: "default"
- **Key**: Id (`BaseBlob<512>`)
- **Value**:
  - Version: uint32 = 1
  - Id: `BaseBlob<512>`
  - Login: string
  - LastAccessTime: Timestamp
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

### 15. ShareLog (statistic.worklog)

Persistent message log for statistics replay on restart.

- **Path**: `{dbPath}/statistic.worklog/*.dat`
- **Owner**: `StatisticDb`
- **Payload type**: CWorkSummaryBatch
  - CWorkSummaryBatch: TimeInterval + `vector<CWorkSummaryEntry>`

### 16. ShareLog (accounting.worklog)

Persistent message log for accounting replay on restart.

- **Path**: `{dbPath}/accounting.worklog/*.dat`
- **Owner**: `AccountingDb`
- **Payload type**: CUserWorkSummaryBatch
  - CUserWorkSummaryBatch: TimeInterval + `vector<CUserWorkSummary>`
    - CUserWorkSummary: UserId (string), AcceptedWork (`UInt<256>`), SharesNum (uint64), BaseBlockReward (`UInt<384>`), ExpectedWork (`UInt<256>`)

### 17. Statistics Cache (.dat files)

In-memory accumulator state persisted to filesystem.

- **Pool stats**: `{dbPath}/stats.pool.cache/{gridEndSec}.dat`
- **User stats**: `{dbPath}/stats.users.cache/{gridEndSec}.dat`
- **Worker stats**: `{dbPath}/stats.workers.cache/{gridEndSec}.dat`
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

---

## Partition Schemes Summary

| Database | Partition Key | Format |
|----------|--------------|--------|
| accounting.state | Single | "default" |
| accounting.rounds | Time (Block.Time) | "YYYY.MM" |
| foundBlocks | Height | height / 1000000 |
| poolBalance | Time | "YYYY.MM" |
| payouts | Time | "YYYY.MM" |
| pplns.payouts | Time (RoundStartTime) | "YYYY.MM" |
| pps.payouts | Time (IntervalBegin) | "YYYY.MM" |
| pps.history | Time | "YYYY.MM" |
| statistic | Time (TimeEnd) | "YYYY.MM" |
| users | Single | "default" |
| userfeeplan | Single | "default" |
| usersettings | Single | "default" |
| useractions | Single | "default" |
| usersessions | Single | "default" |
