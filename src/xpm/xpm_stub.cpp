#include <pthread.h>
#include <unistd.h>
#include "main.h"
#include "wallet.h"

#include "asyncio/asyncio.h"
#include "asyncio/coroutine.h"
#include "asyncio/socket.h"

CBlockIndex *pindexBest;
std::vector<CNode*> vNodes;
CWallet* pwalletMain = 0;
std::map<uint256, CBlockIndex*> mapBlockIndex;
bool fTestNet = true;
CClientUIInterface uiInterface;

int64 GetArg(const std::string& strArg, int64 nDefault) { return nDefault; }

std::string vstrprintf(const char *format, va_list ap)
{
    char buffer[50000];
    char* p = buffer;
    int limit = sizeof(buffer);
    int ret;
    loop
    {
        va_list arg_ptr;
        va_copy(arg_ptr, ap);
#ifdef WIN32
        ret = _vsnprintf(p, limit, format, arg_ptr);
#else
        ret = vsnprintf(p, limit, format, arg_ptr);
#endif
        va_end(arg_ptr);
        if (ret >= 0 && ret < limit)
            break;
        if (p != buffer)
            delete[] p;
        limit *= 2;
        p = new char[limit];
        if (p == NULL)
            throw std::bad_alloc();
    }
    std::string str(p, p+ret);
    if (p != buffer)
        delete[] p;
    return str;
}

std::string real_strprintf(const char *format, int dummy, ...)
{
    va_list arg_ptr;
    va_start(arg_ptr, dummy);
    std::string str = vstrprintf(format, arg_ptr);
    va_end(arg_ptr);
    return str;
}

std::string real_strprintf(const std::string &format, int dummy, ...)
{
    va_list arg_ptr;
    va_start(arg_ptr, dummy);
    std::string str = vstrprintf(format.c_str(), arg_ptr);
    va_end(arg_ptr);
    return str;
}

std::string FormatMoney(int64 n, bool fPlus)
{
    // Note: not using straight sprintf here because we do NOT want
    // localized number formatting.
    int64 n_abs = (n > 0 ? n : -n);
    int64 quotient = n_abs/COIN;
    int64 remainder = n_abs%COIN;
    std::string str = strprintf("%"PRI64d".%08"PRI64d, quotient, remainder);

    // Right-trim excess zeros before the decimal point:
    int nTrim = 0;
    for (int i = str.size()-1; (str[i] == '0' && isdigit(str[i-2])); --i)
        ++nTrim;
    if (nTrim)
        str.erase(str.size()-nTrim, nTrim);

    if (n < 0)
        str.insert((unsigned int)0, 1, '-');
    else if (fPlus && n > 0)
        str.insert((unsigned int)0, 1, '+');
    return str;
}

void CReserveKey::ReturnKey()
{
}

void CScript::SetDestination(const CTxDestination& address)
{
}

bool CBlock::ReadFromDisk(const CBlockIndex* pindex)
{
  return false; 
}

int CMerkleTx::SetMerkleBranch(const CBlock* pblock)
{
  return 0;
}

int CMerkleTx::GetDepthInMainChain(CBlockIndex* &pindexRet) const
{
  return 0;
}

void IncrementExtraNonce(CBlock* pblock, CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
}

CBlockTemplate* CreateNewBlock(CReserveKey& reservekey)
{
  return new CBlockTemplate();
}

int64 CWallet::GetBalance() const
{
  return 1000*COIN;
}

int64 CWallet::GetImmatureBalance() const
{
  return 2000*COIN;  
}

std::string CWallet::SendMoneyToDestination(const CTxDestination &address, int64 nValue, CWalletTx& wtxNew, bool fAskFee)
{
  return "xpmstub can't send money";
}

bool CWallet::CreateTransaction(CScript scriptPubKey, int64 nValue,
                           CWalletTx& wtxNew, CReserveKey& reservekey, int64& nFeeRet, std::string& strFailReason)
{
  return false;
}

bool CWallet::CommitTransaction(CWalletTx& wtxNew, CReserveKey& reservekey)
{
  return false;
}

bool CheckWork(CBlock* pblock, CWallet& wallet, CReserveKey& reservekey)
{
  return false;
}

void newBlockNotify(void *index);

void *poolRpcThread(void *arg);

void timerProc(void *arg)
{
  asyncBase *base = (asyncBase*)arg;
  aioUserEvent *timer = newUserEvent(base, nullptr, nullptr);
  
  while (true) {
    ioSleep(timer, 2000000);
    pindexBest->nHeight++;
    pindexBest->nBits++;
    newBlockNotify(pindexBest);
  }
}

int main(int argc, char **argv)
{
  initializeSocketSubsystem();
  asyncBase *base = createAsyncBase(amOSDefault);
  
  pindexBest = new CBlockIndex;
  
  pindexBest->nBits = 12345;
  pindexBest->nHeight = 777;
  
  coroutineTy *timer = coroutineNew(timerProc, base, 0x10000);
  coroutineCall(timer);
  
  pthread_t thread;
  pthread_create(&thread, 0, poolRpcThread, 0);
  asyncLoop(base);
}
