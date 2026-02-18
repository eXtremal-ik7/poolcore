#!/usr/bin/python3

import json
import requests
import sys

# WARNING: use only on empty database!!!
URL='http://localhost:18880/api'
ADMIN_PASSWORD="admin"
OBSERVER_PASSWORD="observer"

def call(functionName, data, requiredStatus=None):
    result = requests.post(URL + "/" + functionName, json=data).json()
    print("curl -X POST -d '{}' {}/{}\n{}\n".format(json.dumps(data), URL, functionName, json.dumps(result, indent=2)))
    if requiredStatus is not None:
        if result["status"] != requiredStatus:
          raise("{} failed".format(functionName))
    else:
        raise("required status error")
    return result

def userCreateFeePlan(sessionId, feePlanId, requiredStatus=None):
    return call("userCreateFeePlan", {"id": sessionId, "feePlanId": feePlanId}, requiredStatus)

def userQueryFeePlan(sessionId, feePlanId, mode, requiredStatus=None):
    return call("userQueryFeePlan", {"id": sessionId, "feePlanId": feePlanId, "mode": mode}, requiredStatus)

def userUpdateFeePlan(sessionId, feePlanId, mode, default, coinSpecific=None, requiredStatus=None):
    data = {"id": sessionId, "feePlanId": feePlanId, "mode": mode, "default": default}
    if coinSpecific is not None:
        data.update({"coinSpecific": coinSpecific})
    return call("userUpdateFeePlan", data, requiredStatus)

def userDeleteFeePlan(sessionId, feePlanId, requiredStatus=None):
    return call("userDeleteFeePlan", {"id": sessionId, "feePlanId": feePlanId}, requiredStatus)

def userEnumerateFeePlan(sessionId, requiredStatus=None):
    return call("userEnumerateFeePlan", {"id": sessionId}, requiredStatus)

def userChangeFeePlan(sessionId, targetLogin, feePlanId, requiredStatus=None):
    data = {"id": sessionId, "targetLogin": targetLogin, "feePlanId": feePlanId}
    return call("userChangeFeePlan", data, requiredStatus)

def userCreate(name, sessionId=None, isActive=None, isReadOnly=None, feePlan=None, requiredStatus=None):
    data = {"login": name, "password": "12345678", "email": name + "@mail.none"}
    if sessionId is not None:
        data.update({"id": sessionId})
    if isActive is not None:
        data.update({"isActive": isActive})
    if isReadOnly is not None:
        data.update({"isReadOnly": isReadOnly})
    if feePlan is not None:
        data.update({"feePlanId": feePlan})
    return call("userCreate", data, requiredStatus)

def userLogin(name, password, requiredStatus=None):
    return call("userLogin", {"login": name, "password": password}, requiredStatus)

def userGetCredentials(sessionId, targetLogin=None, requiredStatus=None):
    data = {"id": sessionId}
    if targetLogin is not None:
        data.update({"targetLogin": targetLogin})
    return call("userGetCredentials", data, requiredStatus)

def userUpdateCredentials(sessionId, newUserName, targetLogin=None, requiredStatus=None):
    data = {"id": sessionId, "name": newUserName}
    if targetLogin is not None:
        data.update({"targetLogin": targetLogin})
    return call("userUpdateCredentials", data, requiredStatus)

def userUpdateSettings(sessionId, targetLogin=None, payoutThreshold="1", requiredStatus=None):
    data = {"id": sessionId, "coin": "BTC.regtest", "address": "mymcA1ffRQX2f24k74WdpV4WPNPBWsmYFH", "payoutThreshold": payoutThreshold, "autoPayoutEnabled": True}
    if targetLogin is not None:
        data.update({"targetLogin": targetLogin})
    return call("userUpdateSettings", data, requiredStatus)

def userEnumerateAll(sessionId, requiredStatus=None):
    return call("userEnumerateAll", {"id": sessionId}, requiredStatus)

def userGetSettings(sessionId, targetLogin=None, requiredStatus=None):
    data = {"id": sessionId}
    if targetLogin is not None:
        data.update({"targetLogin": targetLogin})
    return call("userGetSettings", data, requiredStatus)

def searchUserRecord(userId, allUserData):
    for record in allUserData:
        if record["login"] == userId:
            return record
    return None

def testUserCreate_1(adminSessionId):
    userCreate("user_not_active",  requiredStatus = "ok")
    userLogin("user_not_active", "12345678", requiredStatus = "user_not_active")

    # admin/observer creation
    userCreate("admin", requiredStatus = "login_format_invalid")
    userCreate("observer", requiredStatus = "login_format_invalid")

    userCreate("user_invalid", isActive=False, isReadOnly=True, requiredStatus = "unknown_id")
    userCreate("user_invalid", None, isActive=True, isReadOnly=False, requiredStatus = "unknown_id")
    userCreate("user_invalid", None, isActive=True, isReadOnly=True, requiredStatus = "unknown_id")

    userCreate("adm1", sessionId=adminSessionId, isActive=True, requiredStatus = "ok")
    userUpdateSettings(adminSessionId, targetLogin="adm1", payoutThreshold="1", requiredStatus = "ok")

    userCreate("adm2", sessionId=adminSessionId, isActive=True, requiredStatus="ok")
    adm2SessionId = userLogin("adm2", "12345678", requiredStatus = "ok")["sessionid"]
    userUpdateSettings(adm2SessionId, payoutThreshold="2", requiredStatus="ok")

    userCreate("adm3", sessionId=adminSessionId, isActive=True, requiredStatus = "ok")
    adm3SessionId = userLogin("adm3", "12345678", requiredStatus = "ok")["sessionid"]
    userUpdateSettings(adm3SessionId, payoutThreshold="3", requiredStatus = "ok")

def testUserUpdateFeePlan(adminSessionId, user1SessionId):
    default = [{"userId": "adm1", "percentage": 4.0}, {"userId": "adm2", "percentage": 4.0}]
    special = [{"userId": "adm1", "percentage": 1.0}, {"userId": "adm2", "percentage": 1.0}, {"userId": "adm3", "percentage": 1.0}]
    extra = [{"userId": "adm1", "percentage": 10}, {"userId": "adm2", "percentage": 10}, {"userId": "adm3", "percentage": 10}]

    # Check that default fee plan is empty
    result = userQueryFeePlan(adminSessionId, "default", "pplns", requiredStatus="ok")
    if result["default"] != [] or result["coinSpecific"] != []:
        raise Exception("default fee plan not empty at startup")

    userUpdateFeePlan(user1SessionId, "default", "pplns", default=[{"userId": "nouser", "percentage": 1.0}], requiredStatus="unknown_id")
    userUpdateFeePlan(adminSessionId, "default", "pplns", default=[{"userId": "nouser", "percentage": 1.0}], requiredStatus="unknown_login")
    userUpdateFeePlan(adminSessionId, "default", "pplns", default=[{"userId": "adm1", "percentage": 4.0}], requiredStatus="ok")

    userUpdateFeePlan(adminSessionId, "default", "pplns", default=default, requiredStatus="ok")

    userCreateFeePlan(adminSessionId, "special", requiredStatus="ok")
    userCreateFeePlan(adminSessionId, "special", requiredStatus="fee_plan_already_exists")
    userUpdateFeePlan(adminSessionId, "special", "pplns", default=special, coinSpecific=[{"coinName": "none", "config": extra}], requiredStatus="invalid_coin")
    userUpdateFeePlan(adminSessionId, "special", "pplns", default=special, coinSpecific=[{"coinName": "LTC.regtest", "config": extra}], requiredStatus="ok")

def testUserEnumerateFeePlan(adminSessionId, user1SessionId):
    userEnumerateFeePlan(user1SessionId, requiredStatus="unknown_id")
    result = userEnumerateFeePlan(adminSessionId, requiredStatus="ok")["plans"]
    if sorted(result) != ["default", "special"]:
        raise Exception("userEnumerateFeePlan error")

def testUserQueryFeePlan(adminSessionId, user1SessionId):
    default = [{"userId": "adm1", "percentage": 4.0}, {"userId": "adm2", "percentage": 4.0}]
    special = [{"userId": "adm1", "percentage": 1.0}, {"userId": "adm2", "percentage": 1.0}, {"userId": "adm3", "percentage": 1.0}]
    extra = [{"userId": "adm1", "percentage": 10}, {"userId": "adm2", "percentage": 10}, {"userId": "adm3", "percentage": 10}]

    userQueryFeePlan(user1SessionId, "default", "pplns", requiredStatus="unknown_id")
    userQueryFeePlan(adminSessionId, "none", "pplns", requiredStatus="unknown_fee_plan")
    result = userQueryFeePlan(adminSessionId, "default", "pplns", requiredStatus="ok")
    if result["default"] != default or result["coinSpecific"] != []:
        raise Exception("userQueryFeePlan error for default plan")

    result = userQueryFeePlan(adminSessionId, "special", "pplns", requiredStatus="ok")
    if result["default"] != special or result["coinSpecific"] != [{"coinName": "LTC.regtest", "config": extra}]:
        raise Exception("userQueryFeePlan error for special plan")

def testUserCreate_2(adminSessionId, adm1SessionId):
    # Create user without session id but with fee plan
    userCreate("user_invalid", feePlan="special", requiredStatus="fee_plan_not_allowed")
    userCreate("user_invalid", sessionId=adm1SessionId, feePlan="special", requiredStatus="fee_plan_not_allowed")
    userCreate("user_invalid", sessionId=adminSessionId, feePlan="none", requiredStatus="fee_plan_not_exists")

    # Create miner1 & miner2 with default fee plan
    userCreate("miner1", feePlan="default", requiredStatus="ok")
    userUpdateSettings(adminSessionId, targetLogin="miner1", payoutThreshold="2", requiredStatus="ok")
    userCreate("miner2", requiredStatus="ok")
    userUpdateSettings(adminSessionId, targetLogin="miner2", payoutThreshold="2", requiredStatus="ok")

    # Create miner3 with special fee plan
    userCreate("miner3", sessionId=adminSessionId, feePlan="special", requiredStatus="ok")
    userUpdateSettings(adminSessionId, targetLogin="miner3", payoutThreshold="2", requiredStatus="ok")

def testUserChangeFeePlan(adminSessionId, adm1SessionId):
    userChangeFeePlan(adm1SessionId, "adm1", "special", "unknown_id")
    userChangeFeePlan(adminSessionId, "nouser", "special", "unknown_id")
    userChangeFeePlan(adminSessionId, "adm1", "none", "fee_plan_not_exists")
    userChangeFeePlan(adminSessionId, "adm1", "special", "ok")
    userChangeFeePlan(adminSessionId, "adm1", "default", "ok")

def testUserUpdateCredentials(adminSessionId, adm1SessionId, adm3SessionId):
    # Update self credentials
    userUpdateCredentials(adm1SessionId, "admin 1", requiredStatus="ok")
    # Update credentials by admin
    userUpdateCredentials(adminSessionId, "miner 1", targetLogin="miner1", requiredStatus="ok")
    # Update non-child user (fail)
    userUpdateCredentials(adm3SessionId, "-", targetLogin="miner1", requiredStatus="unknown_id")
    # Update child user (fail)
    userUpdateCredentials(adm1SessionId, "-", targetLogin="miner1", requiredStatus="unknown_id")

def testUserGetCredentials(adminSessionId, adm1SessionId, adm3SessionId):
    # Get self credentials
    result = userGetCredentials(adm1SessionId, requiredStatus = "ok")["name"]
    if result != "admin 1":
        raise Exception("userGetCredentials failed")

    # Get credentials by admin
    result = userGetCredentials(adminSessionId, targetLogin="miner1", requiredStatus="ok")["name"]
    if result != "miner 1":
        raise Exception("userGetCredentials failed")

    # Get credentials by non-parent user (fail)
    userGetCredentials(adm3SessionId, targetLogin="miner1", requiredStatus="unknown_id")

    # Get credentials by parent user (ok)
    result = userGetCredentials(adm3SessionId, targetLogin="miner3", requiredStatus="ok")["name"]
    if result != "miner3":
        raise Exception("userGetCredentials failed")

def testUserEnumerateAll(adminSessionId, observerSessionId, adm1SessionId, adm3SessionId):
    result = userEnumerateAll(adminSessionId, requiredStatus = "ok")
    if len(result["users"]) != 9:
        raise Exception("userEnumerateAll failed")

    result = userEnumerateAll(observerSessionId, requiredStatus = "ok")
    if len(result["users"]) != 9:
        raise Exception("userEnumerateAll failed")
  
    # check consistency
    miner1Info = searchUserRecord("miner1", result["users"])
    if miner1Info["feePlanId"] != "default":
        raise Exception("userEnumerateAll failed")
    miner2Info = searchUserRecord("miner2", result["users"])
    if miner2Info["feePlanId"] != "default":
        raise Exception("userEnumerateAll failed")
    miner3Info = searchUserRecord("miner3", result["users"])
    if miner3Info["feePlanId"] != "special":
        raise Exception("userEnumerateAll failed")

    ### userEnumerateAll -> adm1 must be [adm1, adm2, adm3, miner1, miner2, miner3, user_not_active]
    result = userEnumerateAll(adm1SessionId, requiredStatus = "ok")
    userList = sorted(list(map(lambda x: x["login"], result["users"])))
    if userList != ['adm1', 'adm2', 'adm3', 'miner1', 'miner2', 'miner3', 'user_not_active']:
        raise("userEnumerateAll failed")

    ### userEnumerateAll -> adm3 must be [miner3]
    result = userEnumerateAll(adm3SessionId, requiredStatus = "ok")
    userList = sorted(list(map(lambda x: x["login"], result["users"])))
    if userList != ['miner3']:
        raise("userEnumerateAll failed")

def testUserGetSettings(adminSessionId, adm1SessionId, adm3SessionId):
    # Get self settings
    userGetSettings(adm1SessionId, requiredStatus = "ok")
    # Get settings by admin
    userGetSettings(adminSessionId, targetLogin="miner1", requiredStatus="ok")
    # Get settings by non-parent user (fail)
    userGetSettings(adm3SessionId, targetLogin="miner1", requiredStatus="unknown_id")
    # Get credentials by parent user (ok)
    userGetSettings(adm3SessionId, targetLogin="miner3", requiredStatus="ok")

# userUpdateSettings
def testUserUpdateSettings(adminSessionId, adm1SessionId, adm3SessionId):
    # Update self settings
    userUpdateSettings(adm1SessionId, payoutThreshold="1.01", requiredStatus="ok")
    # Update settings by admin
    userUpdateSettings(adminSessionId, targetLogin="miner1", payoutThreshold="1.01", requiredStatus="ok")
    # Update non-child user (fail)
    userUpdateSettings(adm3SessionId, targetLogin="miner1", payoutThreshold="1.01", requiredStatus="unknown_id")
    # Update child user (fail)
    userUpdateSettings(adm1SessionId, targetLogin="miner1", payoutThreshold="1.01", requiredStatus="unknown_id")

# Get session id for admin/observer
adminSessionId = userLogin("admin", ADMIN_PASSWORD, requiredStatus="ok")["sessionid"]
observerSessionId = userLogin("observer", OBSERVER_PASSWORD, requiredStatus="ok")["sessionid"]

##### userCreate #####
testUserCreate_1(adminSessionId)
# retrieve session id for users 1-3
adm1SessionId = userLogin("adm1", "12345678", requiredStatus = "ok")["sessionid"]
adm2SessionId = userLogin("adm2", "12345678", requiredStatus = "ok")["sessionid"]
adm3SessionId = userLogin("adm3", "12345678", requiredStatus = "ok")["sessionid"]

##### userUpdateFeePlan #####
testUserUpdateFeePlan(adminSessionId, adm1SessionId)
#### userEnumerateFeePlan #####
testUserEnumerateFeePlan(adminSessionId, adm1SessionId)
##### userQueryFeePlan #####
testUserQueryFeePlan(adminSessionId, adm1SessionId)

##### userCreate 2 #####
testUserCreate_2(adminSessionId, adm1SessionId)

##### userChangeFeePlan #####
testUserChangeFeePlan(adminSessionId, adm1SessionId)

##### userUpdateCredentials #####
testUserUpdateCredentials(adminSessionId, adm1SessionId, adm3SessionId)

##### userGetCredentials #####
testUserGetCredentials(adminSessionId, adm1SessionId, adm3SessionId)

##### userEnumerateAll #####
testUserEnumerateAll(adminSessionId, observerSessionId, adm1SessionId, adm3SessionId)

##### userGetSettings #####
testUserGetSettings(adminSessionId, adm1SessionId, adm3SessionId)

##### userUpdateSettings #####
testUserUpdateSettings(adminSessionId, adm1SessionId, adm3SessionId)
