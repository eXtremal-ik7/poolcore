# Table of contents

* [Common status values suitable for all operations](#common-status-values-suitable-for-all-operations)
* [User management](#user-management)
   * [userChangePasswordInitiate](#userchangepasswordinitiate)
   * [userChangePasswordForce](#userchangepasswordforce)
   * [userCreate](#usercreate)
   * [userResendEmail](#userresendemail)
   * [userAction](#useraction)
   * [userLogin](#userlogin)
   * [userLogout](#userlogout)
   * [userQueryMonitoringSession](#userquerymonitoringsession)
   * [userGetCredentials](#usergetcredentials)
   * [userUpdateCredentials](#userupdatecredentials)
   * [userGetSettings](#usergetsettings)
   * [userUpdateSettings](#userupdatesettings)
   * [userEnumerateAll](#userenumerateall)
   * [userEnumerateFeePlan](#userenumeratefeeplan)
   * [userCreateFeePlan](#usercreatefeeplan)
   * [userQueryFeePlan](#userqueryfeeplan)
   * [userUpdateFeePlan](#userupdatefeeplan)
   * [userDeleteFeePlan](#userdeletefeeplan)
   * [userChangeFeePlan](#userchangefeeplan)
   * [userRenewFeePlanReferralId](#userrenewfeeplanreferralid)
   * [userActivate2faInitiate](#useractivate2fainitiate)
   * [userDeactivate2faInitiate](#userdeactivate2fainitiate)
* [Backend API functions](#backend-api-functions)
   * [backendManualPayout](#backendmanualpayout)
   * [backendQueryCoins](#backendquerycoins)
   * [backendQueryUserBalance](#backendqueryuserbalance)
   * [backendQueryFoundBlocks](#backendqueryfoundblocks)
   * [backendQueryPayouts](#backendquerypayouts)
   * [backendQueryPoolStats](#backendquerypoolstats)
   * [backendQueryPoolStatsHistory](#backendquerypoolstatshistory)
   * [backendQueryUserStats](#backendqueryuserstats)
   * [backendQueryUserStatsHistory](#backendqueryuserstatshistory)
   * [backendQueryWorkerStatsHistory](#backendqueryworkerstatshistory)
   * [backendQueryPPLNSPayouts](#backendquerypplnspayouts)
   * [backendQueryPPLNSAcc](#backendquerypplnsacc)
   * [backendQueryPPSPayouts](#backendqueryppsppayouts)
   * [backendQueryPPSPayoutsAcc](#backendqueryppsppayoutsacc)
   * [backendQueryProfitSwitchCoeff](#backendqueryprofitswitchcoeff)
   * [backendUpdateProfitSwitchCoeff](#backendupdateprofitswitchcoeff)
   * [backendGetConfig](#backendgetconfig)
   * [backendGetPPSState](#backendgetppsstate)
   * [backendQueryPPSHistory](#backendqueryppshistory)
   * [backendUpdateConfig](#backendupdateconfig)
   * [backendPoolLuck](#backendpoolluck)
* [Other API functions](#other-api-functions)
   * [instanceEnumerateAll](#instanceenumerateall)
   * [complexMiningStatsGetInfo](#complexminingstatsgetinfo)

# Common status values suitable for all operations

* ok: operation success
* invalid_json: request is not correct json
* json_format_error: missed argument or argument type mismatch
* request_format_error: invalid function arguments passed

# User management

## userChangePasswordInitiate
Initiate change user's password procedure. if parameter <poolfrontend.smtpEnabled> is "true" in pool config function sends email with link for change password. Link format is http://<poolfrontend.poolHostAddress><poolfrontend.poolChangePasswordLinkPrefix><actionId>, where poolfrontend.poolHostAddress and poolfrontend.poolChangePasswordLinkPrefix defined in pool config; actionId: 512-bit unique identifier of operation, that can be user as input parameter of 'userChangePassword' api.

### arguments:
* login:string - Unique user identifier (up to 64 characters)

### return values:
* status:string - can be one of common status values

### curl example:
```
curl -X POST -d '{"login": "user"}' http://localhost:18880/api/userChangePasswordInitiate
```

### response examples:
```
{"status": "ok"}
```

## userChangePasswordForce
Change user password directly (only for admin account)

### arguments:
* [required] id:string - admin's session id
* [required] login:string - user's login
* [required] newPassword:string - new password

### return values:
* status:string - can be one of common status values

### curl example:
```
curl -X POST -d '{"id": "f3c70b71fe9ad27d2b1861c408058cbc39949a1a3aa834baccdc29721580bc28d6e6a42a5431f023c5031bd6009a1df65d67165b37181b2d991ca3022a703a65", "login": "user", "newPassword": "12345678"}' http://localhost:18880/api/userChangePasswordForce
```

### response examples:
```
{"status": "ok"}
```

## userCreate
Register new user and send email with activation link if parameter <poolfrontend.smtpEnabled> is "true" in pool config. Activation link format is http://<poolfrontend.poolHostAddress><poolfrontend.poolActivateLinkPrefix><actionId>, where poolfrontend.poolHostAddress and poolfrontend.poolActivateLinkPrefix defined in pool config; actionId: 512-bit unique identifier of operation, that can be user as input parameter of 'useraction' api.
Pool frontend must have a handler for configured activation link fornat.

### arguments:
* [required] login:string - Unique user identifier (up to 64 characters)
* [required] password:string - Password (8-64 characters length)
* [optional] email:string - User email, can be omitted, if isActive=true
* [required] name:string - User name (display instead of login in 'blocks found by pool' table (up to 256 characters)
* [optional] isActive:boolean - if true, function will create activated user (option available for admin account only)
* [optional] isReadOnly:boolean - if true, user will have no write access to his account (option available for admin account only)
* [optional] id:string - unique user session id returned by userlogin function, only admin session is usable
* [optional] feePlanId:string - fee plan for user (option available for admin account only)
* [optional] referralId:string - referral ID (hex, 64 chars) to assign fee plan automatically; mutually exclusive with feePlanId

### return values:
* status:string - can be one of common status values or:
  * login_format_invalid
  * password_format_invalid
  * email_format_invalid
  * name_format_invalid
  * duplicate_email: already have another user with requested email
  * duplicate_login: already have user with requested login
  * smtp_client_create_error: internal error with SMTP protocol client
  * email_send_error: error received from SMTP server, details in pool log
  * fee_plan_not_allowed: setup fee plan available only from admin account
  * fee_plan_not_exists: non-existent fee plan sent
  * invalid_referral_id: referral ID not found
  * request_format_error: both feePlanId and referralId specified

### curl example:
```
curl -X POST -d '{"login": "user", "password": "12345678", "email": "my@email.com"}' http://localhost:18880/api/userCreate
curl -X POST -d '{"login": "ro", "password": "12345678", "isActive": true, "isReadOnly": true, "id": "aa342d65135cfb6485c8ca52bacd774418fd1a76fbce5a418ae607a4471c9de0a52e46f36d2b5d1645f83598e34fed7e2750772080122fdaf92becf5e60ed058"}' http://localhost:18880/api/userCreate
curl -X POST -d '{"login": "miner3", "password": "12345678", "email": "miner3@mail.none", "id": "c5a192d62871086fb72bcf736683e0610c486121aeaffb35193af2d63d2144aa8b85a4c56038678de3d8d7c47727e9616d950574dd9b2324e16b49dbeb9f02ad", "feePlanId": "special"}' http://localhost:18880/api/userCreate
```

### response examples:
```
{"status": "ok"}
```
```
{"status": "duplicate_email"}
```

### activation link format example:
```
http://localhost/actions/useracivate?id=6310abb30747f6498a5ec114fdfcc844babdbd9566bcc69e9a2472536a6a850892f339e0866215140497e186710cc15af5582de5e222e4e4a6089dcfd0270017
```

## userResendEmail
Create new user activation id and send it by email. Useful when first email with activation link not received by user.

### arguments:
* login:string - Unique user identifier (up to 64 characters)
* password:string - Password (8-64 characters length)

### return values:
* status:string - can be one of common status values or:
  * invalid_password
  * user_already_active
  * smtp_client_create_error: internal error with SMTP protocol client
  * email_send_error: error received from SMTP server, details in pool log

### curl example:
```
curl -X POST -d "{\"login\": \"user\", \"password\": \"12345678\", \"email\": \"my@email.com\"}" http://localhost:18880/api/userResendEmail
```
### response examples:
```
{"status": "ok"}
```
```
{"status": "user_already_active"}
```

## userAction
Confirm various user actions, such as user activation, changing password, etc

### arguments:
* [required] actionId:string - unique identifier of operation generated by another api function
* [optional] newPassword:string - used for password change action
* [optional] totp:string - used for two factor authentication activation

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid or already activated id
  * unknown_login: internal error
  * password_format_invalid: password does not meet requirements
  * 2fa_invalid: invalid totp code
  * 2fa_already_activated - double 2fa activation
  * 2fa_not_activated - double 2fa deactivation
  * user_already_active: internal error
  * unknown_type: internal error

### curl example:
```
curl -X POST -d '{"actionId": "512e07374333be020565a39be1083f7571a0d8ad0a3eadc9608465a4842b4e5a39384374f91cf540c54979bae0923dbccd667427a26ed8e4913d1f1509ab03ac", "totp": "361607"}' http://localhost:18880/api/userAction
```
### response examples:
```
{"status": "ok"}
```
```
{"status": "unknown_id"}
```

## userLogin
Log in procedure, function accepted login/password and returns session id unique for user

### arguments:
* login:string
* password:string
* totp:string - used only when 2fa for current user activated

### return values:
* sessionid:string: unique session identifier, needed for other api functions
* status:string - can be one of common status values or:
  * invalid_password: login/password mismatch
  * user_not_active: user registered, but not activated using special link sent to email
  * 2fa_invalid: invalid totp code

### curl example:
```
curl -X POST -d "{\"login\": \"user\", \"password\": \"12345678\"}" http://localhost:18880/api/userLogin
```
### responses examples:
```
{"sessionid": "863fe99ef908bc4ba7e954c381224f0370d8840ef6c653b14eba865caafb87c4aa2635312099a72aedc450c8dfa2d87e37641271d927c474b661afc73552d9fc","status": "ok"}
```
```
{"sessionid": "","status": "user_not_active"}
```
```
{"sessionId": "","status": "invalid_password"}
```

## userLogout
Close user session, invalidate session id

### arguments:
* id:string - unique user session id returned by userlogin function

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id

### curl example:
```
curl -X POST -d "{\"id\": \"d6c6a5b8839f4af4eac0e085a25d87efa27c56be6930763877a1410238d6d16b8e83f080719d7d0e6a0a7f927b257f39328ec67922dbdbf5a31c09d9e9413071\"}" http://localhost:18880/api/userLogout
```

### response examples:
```
{"status": "ok"}
```
```
{"status": "unknown_id"}
```

## userQueryMonitoringSession
Create a monitoring (read-only) session for a user

### arguments:
* [required] id:string - unique user session id
* [optional] targetLogin:string - target user login (only for admin session id)

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id
* sessionid:string - monitoring session id

### curl example:
```
curl -X POST -d '{"id": "...session...", "targetLogin": "miner1"}' http://localhost:18880/api/userQueryMonitoringSession
```

### response examples:
```
{"status": "ok", "sessionid": "...monitoring_session_id..."}
```

## userGetCredentials
Query user information

### arguments:
* [required] id:string - unique identifier of operation generated by another api function
* [optional] targetLogin:string - various user login (only for admin session id)

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id
* name:string
* email:string
* registrationDate:integer - uses unix time format

### curl example:
```
curl -X POST -d "{\"id\": \"f8cb3890197f7b4c981e0cec4d28125fca9ea28fe2928121184e1b3a7e4f28048606b6dcead1281b836dafcb540ca507c86d0e2066a595b6fc8c7c3509699c24\"}" http://localhost:18880/api/userGetCredentials
```

### response examples:
```
{"status": "ok","name": "user","email": "my@email.com","registrationDate": 1594539116}
```

## userUpdateCredentials
Function can update only user public name

### arguments:
* [required] id:string - unique identifier of operation generated by another api function
* [optional] targetLogin:string - various user login (only for admin session id)
* [required] name:string - user public name

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id

### curl example:
```
curl -X POST -d '{"id": "af7a1d980afef159bf224c7af676252e924e375f03cbf4b25b999a080829908cc8997b89aca051c444d6e831dcb78230b0369ec8399729d95da09b41e4aed43d", "targetLogin": "user3", "name": "user333"}' http://localhost:18880/api/userUpdateCredentials
```

### response examples:
```
{"status": "ok"}
```

## userGetSettings
Returns user settings for each coin

### arguments
* [required] id:string - unique identifier of operation generated by another api function
* [optional] targetLogin:string - various user login (only for admin session id)

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id
* coins: array of objects with fields:
  * name:string - coin ticker
  * payout: object with fields:
    * address:string - payout address; can be null
    * payoutThreshold:string - minimal value for automatic payout; can be null
    * autoPayoutEnabled:boolean - enables or disables automatic payouts
  * mining: object with fields:
    * mode:string - "pplns" or "pps"
  * autoExchange: object with fields:
    * payoutCoinName:string - coin name for auto-exchange payout; empty string if not set

### curl example:
```
curl -X POST -d '{"id": "...session..."}' http://localhost:18880/api/userGetSettings
```
### response examples:
```
{
  "status": "ok",
  "coins": [
    {
      "name": "BTC",
      "payout": {
        "address": null,
        "payoutThreshold": null,
        "autoPayoutEnabled": false
      },
      "mining": {
        "mode": "pplns"
      },
      "autoExchange": {
        "payoutCoinName": ""
      }
    },
    {
      "name": "XPM",
      "payout": {
        "address": "ATWDYBwVDvswyZADMbEo5yBt4tH2zfGjd1",
        "payoutThreshold": "100.00",
        "autoPayoutEnabled": true
      },
      "mining": {
        "mode": "pplns"
      },
      "autoExchange": {
        "payoutCoinName": ""
      }
    }
  ]
}
```

## userUpdateSettings
Update user settings for specific coin. At least one of payout/mining/autoExchange sub-objects must be provided.

### arguments:
* [required] id:string - unique identifier of operation generated by another api function
* [optional] targetLogin:string - various user login (only for admin session id)
* [required] coin:string
* [optional] totp:string - used only when 2fa for current user activated
* [optional] payout:object - payout settings:
  * [required] address:string - payout address
  * [required] payoutThreshold:string - minimal threshold for automatic payout
  * [required] autoPayoutEnabled:boolean - enable or disable automatic payouts
* [optional] mining:object - mining settings:
  * [required] mode:string - mining mode, "pplns" or "pps"
* [optional] autoExchange:object - auto-exchange settings:
  * [required] payoutCoinName:string - coin name for auto-exchange payout

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id
  * invalid_address: invalid payout address
  * invalid_mining_mode: mode value is not "pplns" or "pps"
  * pps_not_available: PPS mode is not enabled for this coin
  * minimal_payout_too_low: payout threshold is below pool's minimum instant payout

### curl example:
```
curl -X POST -d '{"id": "...session...", "coin": "XPM", "payout": {"address": "ATWDYBwVDvswyZADMbEo5yBt4tH2zfGjd1", "payoutThreshold": "100", "autoPayoutEnabled": true}}' http://localhost:18880/api/userUpdateSettings
curl -X POST -d '{"id": "...session...", "coin": "BTC", "mining": {"mode": "pps"}}' http://localhost:18880/api/userUpdateSettings
```
### response examples:
```
{"status": "ok"}
```

## userEnumerateAll
Returns all registered users for admin/observer account or all 'child' users with personal fee for regular accounts

### arguments:
* [required] id:string - unique identifier of operation generated by another api function
* [required] coin:string
* [optional] offset:integer (default=0) - first row offset
* [optional] size:integer (default=100) - rows count in result
* [optional] sortBy:string (default="averagePower") - column for sorting, can be:
  * login
  * workersNum
  * averagePower
  * sharesPerSecond
  * lastShareTime
* [optional] sortDescending:boolean (default=true) - enable descending sort

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id
  * unknown_column_name: invalid sortBy value
* users - array of credentials objects with these fields:
  * login:string
  * name:string
  * email:string
  * registrationDate:integer - uses unix time format
  * feePlanId:string - fee plan for user
  * workers:integer - number of connections for current user in last N minutes
  * shareRate:float - shares per second
  * power:integer - usually hashrate, depends on coin type
  * lastShareTime:integer - time of last received shared by user

### curl example:
```
curl -X POST -d '{"id": "c26411c326d0e62d02cb0d1614a37eac4e3b848fb37eb7a46f3a2ddceb20a81407a0fa589979efc888c914125c922076552e4ac45324af6a869d8dbbff406422", "coin": "sha256"}' http://localhost:18880/api/userEnumerateAll
```

### response examples:
```
{
  "status": "ok",
  "users": [
    {
      "login": "miner3",
      "name": "miner3",
      "email": "miner3@mail.none",
      "registrationDate": 1622841331,
      "isActive": false,
      "isReadOnly": false,
      "feePlanId": "special",
      "workers": 0,
      "shareRate": 0.0,
      "power": 0,
      "lastShareTime": 0
    }
  ]
}
```

## userEnumerateFeePlan
Returns list of all fee plan IDs (for admin or observer account)

### arguments:
* [required] id:string - unique identifier of operation generated by another api function

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id
* plans:[string] - array of fee plan identifiers

### curl example:
```
curl -X POST -d '{"id": "ADMIN_SESSION_ID"}' http://localhost:18880/api/userEnumerateFeePlan
```

### response examples:
```
{
  "status": "ok",
  "plans": ["default", "special"]
}
```

## userCreateFeePlan
Create new empty fee plan (for admin account only)

### arguments:
* [required] id:string - unique identifier of operation generated by another api function
* [required] feePlanId:string - unique fee plan identifier

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id
  * fee_plan_already_exists: fee plan with this id already exists

### curl example:
```
curl -X POST -d '{"id": "ADMIN_SESSION_ID", "feePlanId": "special"}' http://localhost:18880/api/userCreateFeePlan
```

### response examples:
```
{
  "status": "ok"
}
```

## userQueryFeePlan
Returns fee plan configuration for specified mining mode (for admin or observer account)

### arguments:
* [required] id:string - unique identifier of operation generated by another api function
* [required] feePlanId:string - unique fee plan identifier
* [required] mode:string - mining mode: "pplns" or "pps"

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id
  * unknown_fee_plan: non-existent fee plan requested
  * invalid_mining_mode: invalid mode value
* feePlanId:string - fee plan identifier
* mode:string - mining mode
* referralId:string|null - referral ID (hex, 64 chars) or null if not generated
* default:[UserFeePair] - default fee config ({userId:string, percentage:double})
* coinSpecific:[CoinFeeConfig] - per-coin overrides ({coinName:string, config:[UserFeePair]})

### curl example:
```
curl -X POST -d '{"id": "ADMIN_SESSION_ID", "feePlanId": "special", "mode": "pplns"}' http://localhost:18880/api/userQueryFeePlan
```

### response examples:
```
{
  "status": "ok",
  "feePlanId": "special",
  "mode": "pplns",
  "referralId": "a1b2c3...64hex...",
  "default": [
    {"userId": "adm1", "percentage": 1.0},
    {"userId": "adm2", "percentage": 1.0}
  ],
  "coinSpecific": [
    {
      "coinName": "LTC.regtest",
      "config": [
        {"userId": "adm1", "percentage": 10.0},
        {"userId": "adm2", "percentage": 10.0}
      ]
    }
  ]
}
```

## userUpdateFeePlan
Update fee plan configuration for specified mining mode (for admin account only).
Plan must already exist (use userCreateFeePlan first). Only the specified mode is updated.

### arguments:
* [required] id:string - unique identifier of operation generated by another api function
* [required] feePlanId:string - unique fee plan identifier
* [required] mode:string - mining mode: "pplns" or "pps"
* [required] default:[UserFeePair] - default fee config ({userId:string, percentage:double})
* [optional] coinSpecific:[CoinFeeConfig] - per-coin overrides ({coinName:string, config:[UserFeePair]})

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id
  * unknown_fee_plan: fee plan does not exist
  * invalid_mining_mode: invalid mode value
  * invalid_coin: coin does not exist
  * unknown_login: user referenced in fee config does not exist
  * fee_too_much: total fee percentage >= 100%
  * invalid_percentage: negative percentage value

### curl example:
```
curl -X POST -d '{"id": "ADMIN_SESSION_ID", "feePlanId": "special", "mode": "pplns", "default": [{"userId": "adm1", "percentage": 1.0}, {"userId": "adm2", "percentage": 1.0}], "coinSpecific": [{"coinName": "LTC.regtest", "config": [{"userId": "adm1", "percentage": 10}, {"userId": "adm2", "percentage": 10}]}]}' http://localhost:18880/api/userUpdateFeePlan
```

### response examples:
```
{
  "status": "ok"
}
```

## userDeleteFeePlan
Delete fee plan (for admin account only).
All users assigned to this plan will be reset to "default". Cannot delete the "default" plan.

### arguments:
* [required] id:string - unique identifier of operation generated by another api function
* [required] feePlanId:string - unique fee plan identifier

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id
  * unknown_fee_plan: fee plan does not exist
  * cant_delete_default_plan: cannot delete the default plan

### curl example:
```
curl -X POST -d '{"id": "ADMIN_SESSION_ID", "feePlanId": "special"}' http://localhost:18880/api/userDeleteFeePlan
```

### response examples:
```
{
  "status": "ok"
}
```

## userChangeFeePlan
Change fee plan for specified user (for admin account only)

### arguments:
* [required] id:string - unique identifier of operation generated by another api function
* [optional] targetLogin:string - various user login (only for admin session id)
* [required] feePlanId:string - unique fee plan identifier

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id
  * fee_plan_not_exists: non-existent fee plan sent

### curl example:
```
curl -X POST -d '{"id": "ADMIN_SESSION_ID", "targetLogin": "user1", "feePlanId": "special"}' http://localhost:18880/api/userChangeFeePlan
```

### response examples:
```
{
  "status": "ok"
}
```

## userRenewFeePlanReferralId
Generate (or regenerate) a referral ID for a fee plan. The old referral ID (if any) is invalidated. Access: admin only.

### arguments:
* [required] id:string - admin session id
* [required] feePlanId:string - unique fee plan identifier

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id or insufficient permissions
  * unknown_fee_plan: fee plan does not exist
* referralId:string - new referral ID (hex, 64 chars)

### curl example:
```
curl -X POST -d '{"id": "ADMIN_SESSION_ID", "feePlanId": "special"}' http://localhost:18880/api/userRenewFeePlanReferralId
```

### response examples:
```
{
  "status": "ok",
  "referralId": "a1b2c3d4e5f6...64hex..."
}
```

## userActivate2faInitiate
Activate two factor authentication

### arguments:
* [required] sessionId:string - User session identifier
* [optional] targetLogin:string - various user login (only for admin session id)

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id
  * 2fa_already_activated: double 2fa activation
* key:string - 2fa totp secrey key

### curl example:
```
curl -X POST -d '{"sessionId": "675ea7134fbc88d20763b61912d8aa2f22bab857dfbb1a8c5aacfb5b17b67203fd47215b17c01a42378e2598e0b83ca185c65827a3141cd2d0fec8ee9ef18921"}' http://localhost:18880/api/userActivate2faInitiate
```

### response examples:
```
{
  "status": "ok"
}
```

## userDeactivate2faInitiate
Deactivate two factor authentication

### arguments:
* [required] sessionId:string - User session identifier
* [optional] targetLogin:string - various user login (only for admin session id)

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id
  * 2fa_not_activated: double 2fa deactivation

### curl example:
```
curl -X POST -d '{"sessionId": "675ea7134fbc88d20763b61912d8aa2f22bab857dfbb1a8c5aacfb5b17b67203fd47215b17c01a42378e2598e0b83ca185c65827a3141cd2d0fec8ee9ef18921"}' http://localhost:18880/api/userDeactivate2faInitiate
```

### response examples:
```
{
  "status": "ok"
}
```

# Backend API functions

## backendManualPayout
Force payout all funds from user balance

### arguments:
* [required] id:string - unique identifier of operation generated by another api function
* [optional] targetLogin:string - various user login (only for admin session id)
* [required] coin:string

### return values:
* status:string - can be one of common status values or:
  * payout_error - can't make payout, usually address not set for requested coin
  * insufficient_balance - user non-queued balance less than minimal allowed payout amount
  * no_balance - no balance record found (ok for just registered users)

### curl example:
```
curl -X POST -d '{"id": "ae860bab2faca258c790563a5f97640e55c3c8f23df3fbfde07ed46e201beebbcd04f5b536c5aaf07969c55b09c569c46f37bcfb896c6931be2f9cc4bc6372f8", "coin": "XPM.testnet"}' http://localhost:18880/api/backendManualPayout
```
### response examples:
```
{"status": "ok"}
```

## backendQueryCoins
Function returns coins listed on pool with fee information

### arguments:
* [optional] id:string - user session id; if provided, fees are calculated from the user's fee plan; otherwise the default fee plan is used

### return values:
* status:string - can be one of common status values
* coins: array of objects with these fields:
  * name:string - unique coin id
  * fullName:string - display coin name
  * algorithm:string - mining algorithm
  * minimalPayout:string - minimal allowed payout amount
  * ppsAvailable:boolean - whether PPS mining mode is enabled for this coin
  * pplnsFee:double - total PPLNS fee percentage from user's fee plan
  * ppsFee:double - total PPS fee percentage combining pool PPS fee and user's fee plan

### curl example:
```
curl -X POST -d '{}' http://localhost:18880/api/backendQueryCoins
curl -X POST -d '{"id": "...session..."}' http://localhost:18880/api/backendQueryCoins
```

### response examples:
```
{
  "status": "ok",
  "coins": [
    {
      "name": "BTC",
      "fullName": "Bitcoin",
      "algorithm": "sha256",
      "minimalPayout": "0.001",
      "ppsAvailable": true,
      "pplnsFee": 1.0,
      "ppsFee": 5.0
    },
    {
      "name": "LTC.testnet",
      "fullName": "Litecoin",
      "algorithm": "scrypt",
      "minimalPayout": "0.01",
      "ppsAvailable": false,
      "pplnsFee": 1.0,
      "ppsFee": 5.0
    }
  ]
}
```

## backendQueryUserBalance
Function returns user balance, requested and paid values for one or all available coins

### arguments:
* [required] id:string - unique identifier of operation generated by another api function
* [optional] targetLogin:string - various user login (only for admin session id)
* coin:string

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id
* balances: array of balance objects with fields 'coin', 'balance', 'requested', 'paid' and 'queued'. Field with name 'queued' contains unconfirmed user balance

### curl example:
```
curl -X POST -d '{"id": "7ee12d4f88c54b2a9c850f5d744c1b27cfd5bdf30892e25b197e4c0921b1c9038d17b34e8537f078919b995eab3aae5dab43a944359e40fcffd1171dfceed019"}' http://localhost:18880/api/backendQueryUserBalance
curl -X POST -d '{"id": "7ee12d4f88c54b2a9c850f5d744c1b27cfd5bdf30892e25b197e4c0921b1c9038d17b34e8537f078919b995eab3aae5dab43a944359e40fcffd1171dfceed019", "coin": "XPM.testnet"}' http://localhost:18880/api/backendQueryUserBalance
```

### response examples:
```
{
   "status":"ok",
   "balances":[
      {
         "coin":"XPM.testnet",
         "balance":"29.31",
         "requested":"0.00",
         "paid":"3286.21",
         "queued": "0"
      }
   ]
}
```
```
{
  "status": "ok",
  "balances": [
    {
      "coin": "BTC.regtest",
      "balance": "0.09613028",
      "requested": "0",
      "paid": "0",
      "queued": "0.04577632"
    },
    {
      "coin": "XPM",
      "balance": "0",
      "requested": "0",
      "paid": "0",
      "queued": "0"
    }
  ]
}
```

## backendQueryFoundBlocks:
Function returns blocks found by pool, authorization not required.

### arguments:
* coin:string
* heightFrom:integer (default: -1) - search blocks from this height
* hashFrom:string (default: "") - search blocks from this hash. You need use this 2 arguments for implement page by page loading. With default (or omitted) values search starts from last found block.
* count:integer (default: 20) - requested blocks count
With default arguments function returns last 20 blocks found by pool

### return values:
* status:string - can be one of common status values:
* blocks: array of block objects with fields:
  * height:integer
  * hash:string
  * time:integer (unix time format)
  * confirmations:integer - block confirmations number. -1: means orphan; -2: no data
  * generatedCoins:string - usually first coinbase output value
  * foundBy:string - name (or login) of block founder

### curl example:
```
curl -X POST -d '{"coin": "XPM.testnet", "count": 3}' http://localhost:18880/api/backendQueryFoundBlocks
```

### response examples:
```
{
   "status":"ok",
   "blocks":[
      {
         "height":2466264,
         "hash":"e1d91b43b41ecad70f057b1d7953f8f53ad6c8b9afd7ff78a4bb9f7a8f39526d",
         "time":1595533509,
         "confirmations":107,
         "generatedCoins":"29.32",
         "foundBy":"user"
      },
      {
         "height":2466263,
         "hash":"478546502f05c2622bb597b8d7faee6fe74527738d987a5e225a38f258ceb619",
         "time":1595533507,
         "confirmations":108,
         "generatedCoins":"29.32",
         "foundBy":"user"
      },
      {
         "height":2466262,
         "hash":"c38c1918a136003cc9cd75599acb31bd3dec7b89142792401953a16de879c2e5",
         "time":1595533504,
         "confirmations":109,
         "generatedCoins":"29.32",
         "foundBy":"user"
      }
   ]
}
```

## backendQueryPayouts
Returns sent payouts for user (time and transaction id for each)

### arguments:
* [required] id:string - unique identifier of operation generated by another api function
* [optional] targetLogin:string - various user login (only for admin session id)
* [required] coin:string
* [optional] timeFrom:integer (unix time, default: 0) - search payouts from this time point. You need use this argument for implement page by page loading
* [optional] count:integer (default: 20) - requested payouts count

### return values:
* status:string - can be one of common status values:
* payouts: array of payouts objects with fields:
  * time
  * txid
  * value
  * status (Initialized: 0, TxCreated: 1, TxSent: 2, TxConfirmed: 3, TxRejected: 4)

### curl example:
```
curl -X POST -d '{"id": "8fa732ed14193de6c50b419dcfa1480a3ff6b96208e68a9c1496974a31cc51c035cd3e3708cfca1e50d6a80f2ff77cc4005fea23fa442d6b2dedefdad21d8857", "coin": "XPM.testnet", "count": 3}'
```

### response examples:
```
{
   "status":"ok",
   "payouts":[
      {
         "time":1595534238,
         "txid":"7b026ff4c4088fa94770f3351a07c2af3f593c7f6ffd48bc66da803372f15540",
         "value":"117.29"
      },
      {
         "time":1595533997,
         "txid":"0b331e1f7fde583fd643103e4777ae2f307aca4ec7bde412f0ab05e84ba43a80",
         "value":"117.31"
      },
      {
         "time":1595533637,
         "txid":"e4a2ec354f3abcf0b08f5e78b10dc08b525e22d5399db627606f0c36c7881fa6",
         "value":"117.31"
      }
   ]
}
```

## backendQueryPoolStats
Returns pool statistic for each coin

### arguments:
* [optional] coin:string (default="")

### return values:
* status:string - can be one of common status values
* powerUnit:string - pool power unit (hash/s for BTC, chains per day (cpd) for XPM, etc..)
* powerMultLog10:integer - multiplier for pool hashrate, real power is power*(10^powerMultLog10)
* stats: array of stat objects with fields:
  * coin:string
  * clients:integer - number of client
  * workers:integer - number of workers
  * shareRate:float - shares per second
  * shareWork:float - aggregated work for last N minutes (frontend better to use 'power' field value)
  * power:integer - usually hashrate, depends on coin type
  * lastShareTime:integer - time of last received shared by pool

### curl example:
```
curl -X POST -d '{"coin": "BTC.testnet"}' http://localhost:18880/api/backendQueryPoolStats
```
```
curl -X POST -d '{}' http://localhost:18880/api/backendQueryPoolStats
```

### response examples:
```
{
   "status":"ok",
   "stats":[
      {
         "coin":"BTC.testnet",
         "powerUnit":"hash",
         "powerMultLog10":6,
         "clients":1,
         "workers":1,
         "shareRate":0.024,
         "shareWork":0.800,
         "power":10,
         "lastShareTime":1598655660
      }
   ]
}
```

## backendQueryPoolStatsHistory
Return history pool hashrate on selected time interval

### arguments:
* [required] coin:string
* [optional] timeFrom:integer (default=0) begin of time interval, unix time
* [optional] timeTo:integer (default=UINT64_MAX) end of time interval, unix time
* [optional] groupByInterval:integer (default=3600) grid size

### return values:
* status:string - can be one of common status values
* powerUnit:string - pool power unit (hash/s for BTC, chains per day (cpd) for XPM, etc..)
* powerMultLog10:integer - multiplier for pool hashrate, real power is power*(10^powerMultLog10)
* stats: array of stat objects with fields:
  * name:string - not user
  * time:integer - end of time interval (time-groupByInterval, time]
  * shareRate:float - shares per second
  * shareWork:float - aggregated work in interval (time-groupByInterval, time]
  * power:integer - usually hashrate, depends on coin type

### curl example:
```
curl -X POST -d '{"coin": "BTC.testnet"}' http://localhost:18880/api/backendQueryPoolStatsHistory
```

### response examples:

```
{
   "status":"ok",
   "powerUnit":"hash",
   "powerMultLog10":6,
   "stats":[
      {
         "name":"",
         "time":1598191200,
         "shareRate":0.004,
         "shareWork":4.000,
         "power":4
      },
      {
         "name":"",
         "time":1598194800,
         "shareRate":0.016,
         "shareWork":14.250,
         "power":17
      },
      {
         "name":"",
         "time":1598198400,
         "shareRate":0.017,
         "shareWork":15.500,
         "power":18
      }
   ]
}
```

## backendQueryUserStats
Returns user statistic (aggregate and for each worker)

### arguments:
* [required] id:string - unique identifier of operation generated by another api function
* [optional] targetLogin:string - various user login (only for admin session id)
* [required] coin:string
* [optional] offset:integer (default=0) - first row offset
* [optional] size:integer (default=4096) - rows count in result
* [optional] sortBy:string (default="name) - column for sorting, can be:
  * name
  * averagePower
  * sharesPerSecond
  * lastShareTime
* [optional] sortDescending:boolean (default=false) - enable descending sort

### return values:
* status:string - can be one of common status values or:
  * unknown_column_name - invalid sortBy value
* powerUnit:string - pool power unit (hash/s for BTC, chains per day (cpd) for XPM, etc..)
* powerMultLog10:integer - multiplier for pool hashrate, real power is power*(10^powerMultLog10)
* total: object with these fields:
  * clients:integer - everytime 1
  * workers:integer - number of connections for current user in last N minutes
  * shareRate:float - shares per second
  * shareWork:float - aggregated work for last N minutes
  * power:integer - usually hashrate, depends on coin type
  * lastShareTime:integer - time of last received shared by user
* workers: array of objects with these fields:
  * name:string - worker name
  * shareRate:float - shares per second
  * shareWork:float - aggregated work for last N minutes
  * power:integer - usually hashrate, depends on coin type
  * lastShareTime:integer - time of last received shared by worker

### curl example:
```
curl -X POST -d '{"id": "ae860bab2faca258c790563a5f97640e55c3c8f23df3fbfde07ed46e201beebbcd04f5b536c5aaf07969c55b09c569c46f37bcfb896c6931be2f9cc4bc6372f8", "coin": "BTC.testnet"}' http://localhost:18880/api/backendQueryUserStats
```

### response examples:
```
{
   "status":"ok",
   "powerUnit":"hash",
   "powerMultLog10":6,
   "total":{
      "clients":1,
      "workers":1,
      "shareRate":0.052,
      "shareWork":4.400,
      "power":22,
      "lastShareTime":1598655660
   },
   "workers":[
      {
         "name":"cpu",
         "shareRate":0.052,
         "shareWork":4.400,
         "power":22,
         "lastShareTime":1598655660
      }
   ]
}
```

## backendQueryUserStatsHistory
## backendQueryWorkerStatsHistory
Return history user or worker hashrate on selected time interval

### arguments:
* [required] id:string - unique identifier of operation generated by another api function
* [optional] targetLogin:string - various user login (only for admin session id)
* [required] coin:string
* [required] workerId:string - worker name (only for backendQueryWorkerStatsHistory)
* [optional] timeFrom:integer (default=0) begin of time interval, unix time
* [optional] timeTo:integer (default=UINT64_MAX) end of time interval, unix time
* [optional] groupByInterval:integer (default=3600) grid size

### return values:
* status:string - can be one of common status values
* powerUnit:string - pool power unit (hash/s for BTC, chains per day (cpd) for XPM, etc..)
* powerMultLog10:integer - multiplier for pool hashrate, real power is power*(10^powerMultLog10)
* stats: array of objects with these fields:
  * name:string - not used
  * time:integer - end of time interval (time-groupByInterval, time]
  * shareRate:float - shares per second
  * shareWork:float - aggregated work in interval (time-groupByInterval, time]
  * power:integer - usually hashrate, depends on coin type

### response exapmles:
```
{
   "status":"ok",
   "powerUnit":"hash",
   "powerMultLog10":6,
   "stats":[
      {
         "name":"",
         "time":1598191200,
         "shareRate":0.003,
         "shareWork":2.750,
         "power":3
      },
      {
         "name":"",
         "time":1598194800,
         "shareRate":0.009,
         "shareWork":8.000,
         "power":9
      },
      {
         "name":"",
         "time":1598198400,
         "shareRate":0.021,
         "shareWork":19.250,
         "power":22
      }
   ]
}
```

## backendQueryPPLNSPayouts
Returns PPLNS payout history for user

### arguments:
* [required] id:string - user session id
* [required] coin:string
* [optional] targetLogin:string - target user login (only for admin session id)
* [optional] timeFrom:integer (default=0) - search payouts from this time point (unix time)
* [optional] hashFrom:string (default="") - search payouts from this block hash (for pagination)
* [optional] count:integer (default=20) - requested payouts count

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id
  * invalid_coin: coin does not exist
* payouts: array of objects with fields:
  * startTime:integer - round start time (unix time)
  * endTime:integer - round end time (unix time)
  * hash:string - block hash
  * height:integer - block height
  * value:string - payout value in coin
  * valueBTC:string - payout value in BTC
  * valueUSD:string - payout value in USD

### curl example:
```
curl -X POST -d '{"id": "...session...", "coin": "BTC", "count": 10}' http://localhost:18880/api/backendQueryPPLNSPayouts
```

## backendQueryPPLNSAcc
Returns accumulated PPLNS payouts grouped by time interval

### arguments:
* [required] id:string - user session id
* [required] coin:string
* [required] timeFrom:integer - begin of time interval (unix time)
* [required] timeTo:integer - end of time interval (unix time)
* [required] groupByInterval:integer - grouping interval in seconds
* [optional] targetLogin:string - target user login (only for admin session id)

Constraints: timeTo > timeFrom, groupByInterval > 0, (timeTo - timeFrom) must be divisible by groupByInterval, max 3200 intervals.

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id
  * invalid_coin: coin does not exist
  * invalid_interval: interval constraints violated
* payouts: array of objects with fields:
  * timeLabel:integer - end of interval (unix time)
  * value:string - total payout in coin
  * valueBTC:string - total payout in BTC
  * valueUSD:string - total payout in USD

### curl example:
```
curl -X POST -d '{"id": "...session...", "coin": "BTC", "timeFrom": 1700000000, "timeTo": 1700086400, "groupByInterval": 3600}' http://localhost:18880/api/backendQueryPPLNSAcc
```

## backendQueryPPSPayouts
Returns PPS payout history for user

### arguments:
* [required] id:string - user session id
* [required] coin:string
* [optional] targetLogin:string - target user login (only for admin session id)
* [optional] timeFrom:integer (default=0) - search payouts from this time point (unix time)
* [optional] count:integer (default=20) - requested payouts count

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id
  * invalid_coin: coin does not exist
* payouts: array of objects with fields:
  * startTime:integer - PPS interval start time (unix time)
  * endTime:integer - PPS interval end time (unix time)
  * value:string - payout value in coin
  * valueBTC:string - payout value in BTC
  * valueUSD:string - payout value in USD

### curl example:
```
curl -X POST -d '{"id": "...session...", "coin": "BTC", "count": 10}' http://localhost:18880/api/backendQueryPPSPayouts
```

## backendQueryPPSPayoutsAcc
Returns accumulated PPS payouts grouped by time interval

### arguments:
* [required] id:string - user session id
* [required] coin:string
* [required] timeFrom:integer - begin of time interval (unix time)
* [required] timeTo:integer - end of time interval (unix time)
* [required] groupByInterval:integer - grouping interval in seconds
* [optional] targetLogin:string - target user login (only for admin session id)

Constraints: timeTo > timeFrom, groupByInterval > 0, (timeTo - timeFrom) must be divisible by groupByInterval, max 3200 intervals.

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id
  * invalid_coin: coin does not exist
  * invalid_interval: interval constraints violated
* payouts: array of objects with fields:
  * timeLabel:integer - end of interval (unix time)
  * value:string - total payout in coin
  * valueBTC:string - total payout in BTC
  * valueUSD:string - total payout in USD

### curl example:
```
curl -X POST -d '{"id": "...session...", "coin": "BTC", "timeFrom": 1700000000, "timeTo": 1700086400, "groupByInterval": 3600}' http://localhost:18880/api/backendQueryPPSPayoutsAcc
```

## backendQueryProfitSwitchCoeff
Function returns current profit switcher coefficients, works for admin and observer only

### arguments:
* [required] id:string - unique identifier of operation generated by another api function

### return values:
array of objects with these fields:
* name:string - unique coin id
* profitSwitchCoeff:double - current profit switcher coefficient

### curl example:
```curl -X POST -d '{"id": "bfb3a5e00e52ed152497dd487c7c70571a067ec3c8bc8f4b8c2f17f2f603d9e39ab87a33f8e5533af38879abf94e8c3ab03356b96b8adf8378b1beb46fcbdb32"}' http://localhost:18880/api/backendUpdateProfitSwitchCoeff```

### response exapmle:
```
[
   {
      "name":"BTC",
      "profitSwitchCoeff":1.000
   },
   {
      "name":"DGB.sha256",
      "profitSwitchCoeff":1.000
   },
   {
      "name":"BTC.regtest",
      "profitSwitchCoeff":0.000
   },
   {
      "name":"LTC.testnet",
      "profitSwitchCoeff":0.000
   }
]
```

## backendUpdateProfitSwitchCoeff
Function updates profit switcher coefficients, works for admin only

### arguments:
* [required] id:string - unique identifier of operation generated by another api function
* [required] coin:string
* [required] profitSwitchCoeff:double - new profit switcher coefficient

### return values:
* status:string - can be one of common status values

### curl example:
```
curl -X POST -d '{"id": "bfb3a5e00e52ed152497dd487c7c70571a067ec3c8bc8f4b8c2f17f2f603d9e39ab87a33f8e5533af38879abf94e8c3ab03356b96b8adf8378b1beb46fcbdb32", "coin": "BTC", "profitSwitchCoeff": 0.9}' http://localhost:18880/api/backendUpdateProfitSwitchCoeff
```

### response exapmle:
```
{"status": "ok"}
```

## backendGetConfig
Returns backend configuration (PPS and payouts) for specified coin. Access: admin and observer only.

### arguments:
* [required] id:string - admin or observer session id
* [required] coin:string

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id or insufficient permissions
  * invalid_coin: coin does not exist
* pps: object with fields:
  * enabled:boolean - whether PPS mode is enabled
  * poolFee:double - pool fee percentage
  * saturationFunction:string - saturation function name ("none", "tanh", "clamp", "cubic", "softsign", "norm", "atan", "exp")
  * saturationB0:double - saturation balance scale parameter
  * saturationANegative:double - saturation amplitude for negative balance
  * saturationAPositive:double - saturation amplitude for positive balance
  * saturationFunctions:[string] - list of all supported saturation function names
* payouts: object with fields:
  * instantPayoutsEnabled:boolean - whether instant payouts are enabled
  * regularPayoutsEnabled:boolean - whether regular payouts are enabled
  * instantMinimalPayout:string - minimal payout amount for instant payouts
  * instantPayoutInterval:integer - instant payout check interval in minutes
  * regularMinimalPayout:string - minimal payout amount for regular payouts
  * regularPayoutInterval:integer - regular payout interval in hours
  * regularPayoutDayOffset:integer - regular payout day offset in hours

### curl example:
```
curl -X POST -d '{"id": "...session...", "coin": "BTC"}' http://localhost:18880/api/backendGetConfig
```

### response examples:
```
{
  "status": "ok",
  "pps": {
    "enabled": true,
    "poolFee": 4.0,
    "saturationFunction": "tanh",
    "saturationB0": 3.0,
    "saturationANegative": 0.5,
    "saturationAPositive": 0.3,
    "saturationFunctions": ["none", "tanh", "clamp", "cubic", "softsign", "norm", "atan", "exp"]
  },
  "payouts": {
    "instantPayoutsEnabled": true,
    "regularPayoutsEnabled": true,
    "instantMinimalPayout": "0.01000000",
    "instantPayoutInterval": 1,
    "regularMinimalPayout": "0.00050000",
    "regularPayoutInterval": 24,
    "regularPayoutDayOffset": 0
  }
}
```

## backendGetPPSState
Returns current PPS balance state for specified coin. Access: admin and observer only.

### arguments:
* [required] id:string - admin or observer session id
* [required] coin:string

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id or insufficient permissions
  * invalid_coin: coin does not exist
* state: object with fields:
  * time:integer - timestamp of last state update (unix time)
  * balance:object - current PPS pool balance:
    * value:string - formatted balance
    * inBlocks:double - balance expressed in block rewards
  * refBalance:object - reference balance (not affected by payouts):
    * value:string - formatted balance
    * inBlocks:double - balance expressed in block rewards
    * sqLambda:double - statistical deviation metric (balance / sqrt(totalBlocksFound))
  * minRefBalance:object - minimum reference balance snapshot:
    * time:integer - unix time
    * value:string - formatted balance
    * inBlocks:double
    * sqLambda:double
  * maxRefBalance:object - maximum reference balance snapshot:
    * time:integer - unix time
    * value:string - formatted balance
    * inBlocks:double
    * sqLambda:double
  * totalBlocksFound:double - fractional count of blocks found (PPS portion only)
  * orphanBlocks:double - fractional count of orphaned blocks
  * lastSaturateCoeff:double - last applied saturation coefficient (1.0 = no correction)
  * lastBaseBlockReward:string - base block reward without tx fees (formatted)
  * lastAverageTxFee:string - average transaction fee per block (formatted)

### curl example:
```
curl -X POST -d '{"id": "...session...", "coin": "BTC"}' http://localhost:18880/api/backendGetPPSState
```

### response examples:
```
{
  "status": "ok",
  "state": {
    "time": 1700000000,
    "balance": {
      "value": "0.12345678",
      "inBlocks": 0.020
    },
    "refBalance": {
      "value": "0.15000000",
      "inBlocks": 0.024,
      "sqLambda": 0.145
    },
    "minRefBalance": {
      "time": 1699990000,
      "value": "-0.05000000",
      "inBlocks": -0.008,
      "sqLambda": -0.042
    },
    "maxRefBalance": {
      "time": 1699995000,
      "value": "0.20000000",
      "inBlocks": 0.032,
      "sqLambda": 0.170
    },
    "totalBlocksFound": 3.500,
    "orphanBlocks": 0.000,
    "lastSaturateCoeff": 0.9800,
    "lastBaseBlockReward": "6.25000000",
    "lastAverageTxFee": "0.01500000"
  }
}
```

## backendQueryPPSHistory
Returns PPS balance state history for specified coin and time range. Access: admin and observer only.

### arguments:
* [required] id:string - admin or observer session id
* [required] coin:string
* [required] timeFrom:integer - begin of time interval (unix time)
* [required] timeTo:integer - end of time interval (unix time)

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id or insufficient permissions
  * invalid_coin: coin does not exist
* history: array of state objects (same fields as backendGetPPSState state object)

### curl example:
```
curl -X POST -d '{"id": "...session...", "coin": "BTC", "timeFrom": 1700000000, "timeTo": 1700086400}' http://localhost:18880/api/backendQueryPPSHistory
```

### response examples:
```
{
  "status": "ok",
  "history": [
    {
      "time": 1700000000,
      "balance": { "value": "0.10000000", "inBlocks": 0.016 },
      "refBalance": { "value": "0.12000000", "inBlocks": 0.019, "sqLambda": 0.100 },
      "minRefBalance": { "time": 1699990000, "value": "-0.05000000", "inBlocks": -0.008, "sqLambda": -0.042 },
      "maxRefBalance": { "time": 1699995000, "value": "0.15000000", "inBlocks": 0.024, "sqLambda": 0.130 },
      "totalBlocksFound": 2.500,
      "orphanBlocks": 0.000,
      "lastSaturateCoeff": 1.0000,
      "lastBaseBlockReward": "6.25000000",
      "lastAverageTxFee": "0.01200000"
    },
    {
      "time": 1700003600,
      "balance": { "value": "0.12345678", "inBlocks": 0.020 },
      "refBalance": { "value": "0.15000000", "inBlocks": 0.024, "sqLambda": 0.145 },
      "minRefBalance": { "time": 1699990000, "value": "-0.05000000", "inBlocks": -0.008, "sqLambda": -0.042 },
      "maxRefBalance": { "time": 1700002000, "value": "0.20000000", "inBlocks": 0.032, "sqLambda": 0.170 },
      "totalBlocksFound": 3.500,
      "orphanBlocks": 0.000,
      "lastSaturateCoeff": 0.9800,
      "lastBaseBlockReward": "6.25000000",
      "lastAverageTxFee": "0.01500000"
    }
  ]
}
```

## backendUpdateConfig
Update backend configuration for specified coin. At least one of pps/payouts sub-objects must be provided. Access: admin only.

### arguments:
* [required] id:string - admin session id
* [required] coin:string
* [optional] pps:object - PPS configuration:
  * [required] enabled:boolean - enable or disable PPS mode
  * [required] poolFee:double - pool fee percentage
  * [optional] saturationFunction:string (default="") - saturation function name, empty string means "none"
  * [optional] saturationB0:double (default=0.0) - saturation balance scale; required and must be > 0 when saturation function is not "none"
  * [optional] saturationANegative:double (default=0.0) - saturation amplitude for negative balance; must be in [0, 1]
  * [optional] saturationAPositive:double (default=0.0) - saturation amplitude for positive balance; must be in [0, 1]
* [optional] payouts:object - payouts configuration:
  * [required] instantPayoutsEnabled:boolean - enable or disable instant payouts
  * [required] regularPayoutsEnabled:boolean - enable or disable regular payouts
  * [required] instantMinimalPayout:string - minimal payout amount for instant payouts
  * [required] instantPayoutInterval:integer - instant payout check interval in minutes (must be > 0)
  * [required] regularMinimalPayout:string - minimal payout amount for regular payouts
  * [required] regularPayoutInterval:integer - regular payout interval in hours (must be > 0; must divide 24 or be a multiple of 24)
  * [optional] regularPayoutDayOffset:integer (default=0) - regular payout day offset in hours (must be in [0, 24))
* [optional] adjustUserInstantMinimalPayout:boolean (default=false) - if true, adjust all users' minimal payout to be at least the new instantMinimalPayout

### return values:
* status:string - can be one of common status values or:
  * unknown_id: invalid session id or insufficient permissions
  * invalid_coin: coin does not exist
  * invalid_pool_fee: pool fee out of [0, 100] range
  * invalid_saturation_function: unknown saturation function name
  * invalid_saturation_params: B0 <= 0, or A values out of [0, 1] range
  * invalid_payout_interval: interval is zero or negative
  * invalid_regular_payout_interval: interval does not divide 24 and is not a multiple of 24
  * invalid_regular_payout_day_offset: offset >= 24 hours

### curl example:
```
curl -X POST -d '{"id": "...session...", "coin": "BTC", "pps": {"enabled": true, "poolFee": 4.0, "saturationFunction": "tanh", "saturationB0": 3.0, "saturationANegative": 0.5, "saturationAPositive": 0.3}}' http://localhost:18880/api/backendUpdateConfig
curl -X POST -d '{"id": "...session...", "coin": "BTC", "payouts": {"instantPayoutsEnabled": true, "regularPayoutsEnabled": true, "instantMinimalPayout": "0.01", "instantPayoutInterval": 1, "regularMinimalPayout": "0.0005", "regularPayoutInterval": 24, "regularPayoutDayOffset": 0}}' http://localhost:18880/api/backendUpdateConfig
```

### response examples:
```
{"status": "ok"}
```

## backendPoolLuck
Returns pool luck values for specified time intervals. No authentication required.

### arguments:
* [required] coin:string
* [required] intervals:[integer] - array of time intervals in seconds, must be strictly increasing

### return values:
* status:string - can be one of common status values or:
  * invalid_coin: coin does not exist
* luck:[double] - array of luck values corresponding to each interval (1.0 = expected, <1.0 = lucky)

### curl example:
```
curl -X POST -d '{"coin": "BTC", "intervals": [3600, 86400, 604800]}' http://localhost:18880/api/backendPoolLuck
```

### response examples:
```
{
  "status": "ok",
  "luck": [1.25, 0.98, 1.02]
}
```

# Other API functions

## instanceEnumerateAll
Function returns information about all instances (stratum/other ports, configuration)

### arguments:
none

### return values:
* status:string - "ok"
* instances:array - array of objects with these fields:
  * protocol:string - usual 'stratum'
  * type:string - protocol implementation, usual a coin name
  * port:integer
  * backends:[string] - array of available backends for mining using this instance
  * shareDiff:float - only for BTC-like stratum instances, minimal share difficulty

### curl example:
```
curl -X POST -d '{}' http://localhost:18880/api/instanceEnumerateAll
```

### response exapmle:
```
{
   "status":"ok",
   "instances":[
      {
         "protocol":"stratum",
         "type":"BTC",
         "port":3456,
         "backends":[
            "BTC"
         ],
         "shareDiff":32768.000000
      },
      {
         "protocol":"stratum",
         "type":"LTC",
         "port":3460,
         "backends":[
            "LTC.testnet"
         ],
         "shareDiff":0.000010
      }
   ]
}
```

## complexMiningStatsGetInfo
Returns complex mining statistics information. Access: admin only.

### arguments:
* [required] id:string - admin session id

### return values:
* Response format depends on ComplexMiningStats implementation

### curl example:
```
curl -X POST -d '{"id": "...session..."}' http://localhost:18880/api/complexMiningStatsGetInfo
```
