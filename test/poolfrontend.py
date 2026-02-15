import json
import requests
import sys

class Poolfrontend:
    URL=""

    def __init__(self, url):
        self.URL = url

    def __call__(self, functionName, data, requiredStatus=None, debug=None):
        print(requiredStatus)
        result = requests.post(self.URL + "/" + functionName, json=data).json()
        if debug is True:
            print("curl -X POST -d '{}' {}/{}\n{}\n".format(json.dumps(data), self.URL, functionName,
                                                            json.dumps(result, indent=2)))
        if requiredStatus is not None:
            if result["status"] != requiredStatus:
                raise Exception("{} failed".format(functionName))
        return result

    def userChangePasswordInitiate(self, login, requiredStatus=None, debug=None):
        return self.__call__("userChangePasswordInitiate", {"login": login}, requiredStatus, debug)

    def userChangePasswordForce(self, adminSessionId, login, newPassword, requiredStatus=None, debug=None):
        return self.__call__("userChangePasswordForce", {"id": adminSessionId, "login": login, "newPassword": newPassword}, requiredStatus, debug)

    def userCreate(self, name, password, email, sessionId=None, isActive=None, isReadOnly=None, feePlan=None, requiredStatus=None, debug=None):
        data = {"login": name, "password": password, "email": email}
        if sessionId is not None:
            data.update({"id": sessionId})
        if isActive is not None:
            data.update({"isActive": isActive})
        if isReadOnly is not None:
            data.update({"isReadOnly": isReadOnly})
        if feePlan is not None:
            data.update({"feePlanId": feePlan})
        return self.__call__("userCreate", data, requiredStatus, debug)

    def userResendEmail(self, login, password, requiredStatus=None, debug=None):
        return self.__call__("userResendEmail", {"login": login, "password": password}, requiredStatus, debug)

    def userAction(self, actionId, newPassword=None, totp=None, requiredStatus=None, debug=None):
        data = {"actionId": actionId}
        if newPassword is not None:
            data.update({"newPassword": newPassword})
        if totp is not None:
            data.update({"totp": totp})
        return self.__call__("userAction", data, requiredStatus, debug)

    def userLogin(self, name, password, totp=None, requiredStatus=None, debug=None):
        data = {"login": name, "password": password}
        if totp is not None:
            data.update({"totp": totp})
        return self.__call__("userLogin", data, requiredStatus, debug)

    def userLogout(self, sessionId, requiredStatus=None, debug=None):
        return self.__call__("userLogout", {"id": sessionId}, requiredStatus, debug)

    def userGetCredentials(self, sessionId, targetLogin=None, requiredStatus=None, debug=None):
        data = {"id": sessionId}
        if targetLogin is not None:
            data.update({"targetLogin": targetLogin})
        return self.__call__("userGetCredentials", data, requiredStatus, debug)

    def userUpdateCredentials(self, sessionId, newName, targetLogin=None, requiredStatus=None, debug=None):
        data = {"id": sessionId, "name": newName}
        if targetLogin is not None:
            data.update({"targetLogin": targetLogin})
        return self.__call__("userUpdateCredentials", data, requiredStatus, debug)

    def userGetSettings(self, sessionId, targetLogin=None, requiredStatus=None, debug=None):
        data = {"id": sessionId}
        if targetLogin is not None:
            data.update({"targetLogin": targetLogin})
        return self.__call__("userGetSettings", data, requiredStatus, debug)

    def userUpdateSettings(self, sessionId, coin, address, payoutThreshold, autoPayoutEnabled, targetLogin=None, totp=None, requiredStatus=None, debug=None):
        data = {"id": sessionId, "coin": coin, "address": address, "payoutThreshold": payoutThreshold, "autoPayoutEnabled": autoPayoutEnabled}
        if targetLogin is not None:
            data.update({"targetLogin": targetLogin})
        if totp is not None:
            data.update({"totp": totp})
        return self.__call__("userUpdateSettings", data, requiredStatus, debug)

    def userEnumerateAll(self, sessionId, coin, offset=None, size=None, sortBy=None, sortDescending=None, requiredStatus=None, debug=None):
        data = {"id": sessionId, "coin": coin}
        if offset is not None:
            data.update({"offset": offset})
        if size is not None:
            data.update({"size": size})
        if sortBy is not None:
            data.update({"sortBy": sortBy})
        if sortDescending is not None:
            data.update({"sortDescending": sortDescending})
        return self.__call__("userEnumerateAll", data, requiredStatus, debug)

    def userEnumerateFeePlan(self, adminSessionId, requiredStatus=None, debug=None):
        return self.__call__("userEnumerateFeePlan", {"id": adminSessionId}, requiredStatus, debug)

    def userGetFeePlan(self, adminSessionId, feePlanId, requiredStatus=None, debug=None):
        return self.__call__("userGetFeePlan", {"id": adminSessionId, "feePlanId": feePlanId}, requiredStatus, debug)

    # example:
    #  pool.userUpdateFeePlan(adminSessionId, "default", [{"userId": "user1", "percentage": 5.0}], coinSpecificFee=[{"coin": "BTC.regtest", "config": [{"userId": "user1", "percentage": 3.0}]}])
    def userUpdateFeePlan(self, adminSessionId, feePlanId, default, coinSpecificFee=None, requiredStatus=None, debug=None):
        data = {"id": adminSessionId, "feePlanId": feePlanId, "default": default}
        if coinSpecificFee is not None:
            data.update({"coinSpecificFee": coinSpecificFee})
        return self.__call__("userUpdateFeePlan", data, requiredStatus, debug)

    def userChangeFeePlan(self, adminSessionId, targetLogin, feePlanId, requiredStatus=None, debug=None):
        return self.__call__("userChangeFeePlan", {"id": adminSessionId, "targetLogin": targetLogin, "feePlanId": feePlanId}, requiredStatus, debug)

    def userActivate2faInitiate(self, sessionId, targetLogin=None, requiredStatus=None, debug=None):
        data = {"sessionId": sessionId}
        if targetLogin is not None:
            data.update({"targetLogin": targetLogin})
        return self.__call__("userActivate2faInitiate", data, requiredStatus, debug)

    def userDeactivate2faInitiate(self, sessionId, targetLogin=None, requiredStatus=None, debug=None):
        data = {"sessionId": sessionId}
        if targetLogin is not None:
            data.update({"targetLogin": targetLogin})
        return self.__call__("userDeactivate2faInitiate", data, requiredStatus, debug)

    def backendManualPayout(self, sessionId, coin, targetLogin=None, requiredStatus=None, debug=None):
        data = {"id": sessionId, "coin": coin}
        if targetLogin is not None:
            data.update({"targetLogin": targetLogin})
        return self.__call__("backendManualPayout", data, requiredStatus, debug)

    def backendQueryCoins(self, requiredStatus=None, debug=None):
        return self.__call__("backendQueryCoins", {}, requiredStatus, debug)

    def backendQueryUserBalance(self, sessionId, coin=None, targetLogin=None, requiredStatus=None, debug=None):
        data = {"id": sessionId}
        if coin is not None:
            data.update({"coin": coin})
        if targetLogin is not None:
            data.update({"targetLogin": targetLogin})
        return self.__call__("backendQueryUserBalance", data, requiredStatus, debug)

    def backendQueryFoundBlocks(self, coin, heightFrom=None, hashFrom=None, count=None, requiredStatus=None, debug=None):
        data = {"coin": coin}
        if heightFrom is not None:
            data.update({"heightFrom": heightFrom})
        if hashFrom is not None:
            data.update({"hashFrom": hashFrom})
        if count is not None:
            data.update({"count": count})
        return self.__call__("backendQueryFoundBlocks", data, requiredStatus, debug)

    def backendQueryPayouts(self, sessionId, coin, targetLogin=None, timeFrom=None, count=None, requiredStatus=None, debug=None):
        data = {"id": sessionId, "coin": coin}
        if targetLogin is not None:
            data.update({"targetLogin": targetLogin})
        if timeFrom is not None:
            data.update({"timeFrom": timeFrom})
        if count is not None:
            data.update({"count": count})
        return self.__call__("backendQueryPayouts", data, requiredStatus, debug)

    def backendQueryPoolStats(self, coin=None, requiredStatus=None, debug=None):
        data = {}
        if coin is not None:
            data.update({"coin": coin})
        return self.__call__("backendQueryPoolStats", data, requiredStatus, debug)

    def backendQueryPoolStatsHistory(self, coin, timeFrom=None, timeTo=None, groupByInterval=None, requiredStatus=None, debug=None):
        data = {"coin": coin}
        if timeFrom is not None:
            data.update({"timeFrom": timeFrom})
        if timeTo is not None:
            data.update({"timeTo": timeTo})
        if groupByInterval is not None:
            data.update({"groupByInterval": groupByInterval})
        return self.__call__("backendQueryPoolStatsHistory", data, requiredStatus, debug)

    def backendQueryUserStats(self, sessionId, coin, targetLogin=None, offset=None, size=None, sortBy=None, sortDescending=None, requiredStatus=None, debug=None):
        data = {"id": sessionId, "coin": coin}
        if targetLogin is not None:
            data.update({"targetLogin": targetLogin})
        if offset is not None:
            data.update({"offset": offset})
        if size is not None:
            data.update({"size": size})
        if sortBy is not None:
            data.update({"sortBy": sortBy})
        if sortDescending is not None:
            data.update({"sortDescending": sortDescending})
        return self.__call__("backendQueryUserStats", data, requiredStatus, debug)

    def backendQueryUserStatsHistory(self, sessionId, coin, targetLogin=None, timeFrom=None, timeTo=None, groupByInterval=None, requiredStatus=None, debug=None):
        data = {"id": sessionId, "coin": coin}
        if targetLogin is not None:
            data.update({"targetLogin": targetLogin})
        if timeFrom is not None:
            data.update({"timeFrom": timeFrom})
        if timeTo is not None:
            data.update({"timeTo": timeTo})
        if groupByInterval is not None:
            data.update({"groupByInterval": groupByInterval})
        return self.__call__("backendQueryUserStatsHistory", data, requiredStatus, debug)

    def backendQueryWorkerStatsHistory(self, sessionId, coin, workerId, targetLogin=None, timeFrom=None, timeTo=None, groupByInterval=None, requiredStatus=None, debug=None):
        data = {"id": sessionId, "coin": coin, "workerId": workerId}
        if targetLogin is not None:
            data.update({"targetLogin": targetLogin})
        if timeFrom is not None:
            data.update({"timeFrom": timeFrom})
        if timeTo is not None:
            data.update({"timeTo": timeTo})
        if groupByInterval is not None:
            data.update({"groupByInterval": groupByInterval})
        return self.__call__("backendQueryWorkerStatsHistory", data, requiredStatus, debug)

    def backendQueryProfitSwitchCoeff(self, adminSessionId, requiredStatus=None, debug=None):
        return self.__call__("backendQueryProfitSwitchCoeff", {"id": adminSessionId}, requiredStatus, debug)

    def backendUpdateProfitSwitchCoeff(self, adminSessionId, coin, profitSwitchCoeff, requiredStatus=None, debug=None):
        return self.__call__("backendUpdateProfitSwitchCoeff", {"id": adminSessionId, "coin": coin, "profitSwitchCoeff": profitSwitchCoeff}, requiredStatus, debug)

    def backendPoolLuck(self, coin, intervals, requiredStatus=None, debug=None):
        return self.__call__("backendPoolLuck", {"coin": coin, "intervals": intervals}, requiredStatus, debug)

    def instanceEnumerateAll(self, requiredStatus=None, debug=None):
        return self.__call__("instanceEnumerateAll", {}, requiredStatus, debug)
