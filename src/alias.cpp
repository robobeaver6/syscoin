// Copyright (c) 2014 The Syscoin Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.
//
#include "alias.h"
#include "offer.h"
#include "escrow.h"
#include "cert.h"
#include "offer.h"
#include "asset.h"
#include "assetallocation.h"
#include "init.h"
#include "validation.h"
#include "util.h"
#include "random.h"
#include "wallet/wallet.h"
#include "rpc/client.h"
#include "rpc/server.h"
#include "base58.h"
#include "txmempool.h"
#include "txdb.h"
#include "chainparams.h"
#include "core_io.h"
#include "policy/policy.h"
#include "utiltime.h"
#include "wallet/coincontrol.h"
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/case_conv.hpp> // for to_lower()
#include <boost/xpressive/xpressive_dynamic.hpp>
#include <boost/foreach.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/thread.hpp>
#include <boost/algorithm/hex.hpp>
#include <boost/algorithm/string/find.hpp>
#include <boost/assign/list_of.hpp>
#include "instantx.h"
using namespace std;
CAliasDB *paliasdb = NULL;
COfferDB *pofferdb = NULL;
CCertDB *pcertdb = NULL;
CEscrowDB *pescrowdb = NULL;
CAssetDB *passetdb = NULL;
CAssetAllocationDB *passetallocationdb = NULL;
typedef map<vector<unsigned char>, COutPoint > mapAliasRegistrationsType;
typedef map<vector<unsigned char>, vector<unsigned char> > mapAliasRegistrationsDataType;
mapAliasRegistrationsType mapAliasRegistrations;
mapAliasRegistrationsDataType mapAliasRegistrationData;
unsigned int MAX_ALIAS_UPDATES_PER_BLOCK = 10;
uint64_t GetAliasExpiration(const CAliasIndex& alias) {
	// dont prune by default, set nHeight to future time
	uint64_t nTime = chainActive.Tip()->GetMedianTimePast() + 1;
	CAliasUnprunable aliasUnprunable;
	// if service alias exists in unprunable db (this should always exist for any alias that ever existed) then get the last expire height set for this alias and check against it for pruning
	if (paliasdb && paliasdb->ReadAliasUnprunable(alias.vchAlias, aliasUnprunable) && !aliasUnprunable.IsNull())
		nTime = aliasUnprunable.nExpireTime;
	return nTime;
}
bool GetTimeToPrune(const CScript& scriptPubKey, uint64_t &nTime)
{
	vector<unsigned char> vchData;
	vector<unsigned char> vchHash;
	if(!GetSyscoinData(scriptPubKey, vchData, vchHash))
		return false;
	if(!chainActive.Tip())
		return false;
	CAliasIndex alias;
	COffer offer;
	CEscrow escrow;
	CCert cert;
	CAssetAllocation assetallocation;
	CAsset asset;
	nTime = 0;
	if(alias.UnserializeFromData(vchData, vchHash))
	{
		CAliasUnprunable aliasUnprunable;
		// we only prune things that we have in our db and that we can verify the last tx is expired
		// nHeight is set to the height at which data is pruned, if the tip is newer than nHeight it won't send data to other nodes
		// we want to keep history of all of the old tx's related to aliases that were renewed, we can't delete its history otherwise we won't know 
		// to tell nodes that aliases were renewed and to update their info pertaining to that alias.
		if (paliasdb && paliasdb->ReadAliasUnprunable(alias.vchAlias, aliasUnprunable) && !aliasUnprunable.IsNull())
		{
			// if we are renewing alias then prune based on max of expiry of alias in tx vs the stored alias expiry time of latest alias tx
			if (!alias.vchGUID.empty() && aliasUnprunable.vchGUID != alias.vchGUID)
				nTime = max(alias.nExpireTime, aliasUnprunable.nExpireTime);
			else
				nTime = aliasUnprunable.nExpireTime;

			return true;
		}
		// this is a new service, either sent to us because it's not supposed to be expired yet or sent to ourselves as a new service, either way we keep the data and validate it into the service db
		else
		{
			// setting to the tip means we don't prune this data, we keep it
			nTime = chainActive.Tip()->GetMedianTimePast() + 1;
			return true;
		}
	}
	else if(offer.UnserializeFromData(vchData, vchHash))
	{
		if (!pofferdb || !pofferdb->ReadOffer(offer.vchOffer, offer))
		{
			// setting to the tip means we don't prune this data, we keep it
			nTime = chainActive.Tip()->GetMedianTimePast() + 1;
			return true;
		}
		nTime = GetOfferExpiration(offer);
		return true; 
	}
	else if(cert.UnserializeFromData(vchData, vchHash))
	{
		if (!pcertdb || !pcertdb->ReadCert(cert.vchCert, cert))
		{
			// setting to the tip means we don't prune this data, we keep it
			nTime = chainActive.Tip()->GetMedianTimePast() + 1;
			return true;
		}
		nTime = GetCertExpiration(cert);
		return true; 
	}
	else if(escrow.UnserializeFromData(vchData, vchHash))
	{
		if (!pescrowdb || !pescrowdb->ReadEscrow(escrow.vchEscrow, escrow))
		{
			// setting to the tip means we don't prune this data, we keep it
			nTime = chainActive.Tip()->GetMedianTimePast() + 1;
			return true;
		}
		nTime = GetEscrowExpiration(escrow);
		return true;
	}
	return false;
}
bool IsSysServiceExpired(const uint64_t &nTime)
{
	if(!chainActive.Tip())
		return false;
	return (chainActive.Tip()->GetMedianTimePast() >= nTime);

}
bool IsSyscoinScript(const CScript& scriptPubKey, int &op, vector<vector<unsigned char> > &vvchArgs)
{
	if (DecodeAliasScript(scriptPubKey, op, vvchArgs))
		return true;
	else if(DecodeOfferScript(scriptPubKey, op, vvchArgs))
		return true;
	else if(DecodeCertScript(scriptPubKey, op, vvchArgs))
		return true;
	else if(DecodeEscrowScript(scriptPubKey, op, vvchArgs))
		return true;
	else if (DecodeAssetScript(scriptPubKey, op, vvchArgs))
		return true;
	else if (DecodeAssetAllocationScript(scriptPubKey, op, vvchArgs))
		return true;
	return false;
}
bool RemoveSyscoinScript(const CScript& scriptPubKeyIn, CScript& scriptPubKeyOut)
{
	if (!RemoveAliasScriptPrefix(scriptPubKeyIn, scriptPubKeyOut))
		if (!RemoveOfferScriptPrefix(scriptPubKeyIn, scriptPubKeyOut))
			if (!RemoveCertScriptPrefix(scriptPubKeyIn, scriptPubKeyOut))
				if (!RemoveEscrowScriptPrefix(scriptPubKeyIn, scriptPubKeyOut))
					if (!RemoveAssetScriptPrefix(scriptPubKeyIn, scriptPubKeyOut))
						if (!RemoveAssetAllocationScriptPrefix(scriptPubKeyIn, scriptPubKeyOut))
							return false;
	return true;
					
}
float getEscrowFee()
{
	// 0.05% escrow fee
	return 0.005;
}
int getFeePerByte(const uint64_t &paymentOptionMask)
{   
	if (IsPaymentOptionInMask(paymentOptionMask, PAYMENTOPTION_BTC))
		return 250;
	else  if (IsPaymentOptionInMask(paymentOptionMask, PAYMENTOPTION_SYS))
		return 25;
	else  if (IsPaymentOptionInMask(paymentOptionMask, PAYMENTOPTION_ZEC))
		return 25;
	return 25;
}

bool IsAliasOp(int op) {
		return op == OP_ALIAS_ACTIVATE
			|| op == OP_ALIAS_UPDATE;
}
string aliasFromOp(int op) {
	switch (op) {
	case OP_ALIAS_UPDATE:
		return "aliasupdate";
	case OP_ALIAS_ACTIVATE:
		return "aliasactivate";
	default:
		return "<unknown alias op>";
	}
}
int GetSyscoinDataOutput(const CTransaction& tx) {
   for(unsigned int i = 0; i<tx.vout.size();i++) {
	   if(IsSyscoinDataOutput(tx.vout[i]))
		   return i;
	}
   return -1;
}
bool IsSyscoinDataOutput(const CTxOut& out) {
   txnouttype whichType;
	if (!IsStandard(out.scriptPubKey, whichType))
		return false;
	if (whichType == TX_NULL_DATA)
		return true;
   return false;
}


bool CheckAliasInputs(const CTransaction &tx, int op, const vector<vector<unsigned char> > &vvchArgs, bool fJustCheck, int nHeight, string &errorMessage, bool &bDestCheckFailed, bool bSanityCheck) {
	if (!paliasdb)
		return false;
	if (tx.IsCoinBase() && !fJustCheck && !bSanityCheck)
	{
		LogPrintf("*Trying to add alias in coinbase transaction, skipping...");
		return true;
	}
	if (fDebug && !bSanityCheck)
		LogPrintf("*** ALIAS %d %d op=%s %s %s\n", nHeight, chainActive.Tip()->nHeight, aliasFromOp(op).c_str(), tx.GetHash().ToString().c_str(), fJustCheck ? "JUSTCHECK" : "BLOCK");
	// alias registration has args size of 1 we don't care to validate it until the activation comes in with args size of 4
	if (vvchArgs.size() < 4)
		return true;
	
	bDestCheckFailed = true;
	int prevOp = 0;
	vector<vector<unsigned char> > vvchPrevArgs;

	// unserialize alias from txn, check for valid
	CAliasIndex theAlias;
	vector<unsigned char> vchData;
	vector<unsigned char> vchAlias;
	vector<unsigned char> vchHash;
	int nDataOut;
	bool bData = GetSyscoinData(tx, vchData, vchHash, nDataOut);
	if(bData && !theAlias.UnserializeFromData(vchData, vchHash))
	{
		theAlias.SetNull();
	}
	if(fJustCheck)
	{
		
		if(vvchArgs.size() != 4)
		{
			errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5000 - " + _("Alias arguments incorrect size");
			return error(errorMessage.c_str());
		}
		
		if(!theAlias.IsNull())
		{
			if(vvchArgs.size() <= 2 || vchHash != vvchArgs[2])
			{
				errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5001 - " + _("Hash provided doesn't match the calculated hash of the data");
				return true;
			}
		}					
		
	}
	// MAX_ALIAS_UPDATES_PER_BLOCK + 1(change address) + 2(data output and alias coloured output) + 1(alias transfer potentially)
	if (tx.vout.size() > (MAX_ALIAS_UPDATES_PER_BLOCK + 4))
	{
		errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5002 - " + _("Too many outputs for this Syscoin transaction");
		return error(errorMessage.c_str());
	}
	Coin prevCoins;
	if(fJustCheck || op != OP_ALIAS_ACTIVATE)
	{
		// Strict check - bug disallowed
		for (unsigned int i = 0; i < tx.vin.size(); i++) {
			vector<vector<unsigned char> > vvch;
			int pop;
			if (!GetUTXOCoin(tx.vin[i].prevout, prevCoins))
				continue;
			// ensure inputs are unspent when doing consensus check to add to block
			if(!DecodeAliasScript(prevCoins.out.scriptPubKey, pop, vvch))
			{
				continue;
			}
			if (op == OP_ALIAS_ACTIVATE || (vvchArgs.size() > 1 && vvchArgs[0] == vvch[0] && vvchArgs[1] == vvch[1])) {
				prevOp = pop;
				vvchPrevArgs = vvch;
				break;
			}
		}
		if(vvchArgs.size() >= 4 && !vvchArgs[3].empty())
		{
			bool bWitnessSigFound = false;
			for (unsigned int i = 0; i < tx.vin.size(); i++) {
				vector<vector<unsigned char> > vvch;
				int pop;
				Coin prevCoins;
				if (!GetUTXOCoin(tx.vin[i].prevout, prevCoins))
					continue;
				// ensure inputs are unspent when doing consensus check to add to block
				if (!DecodeAliasScript(prevCoins.out.scriptPubKey, pop, vvch))
				{
					continue;
				}
				// match 4th element in scriptpubkey of alias update with witness input scriptpubkey, if names match then sig is provided
				if (vvchArgs[3] == vvch[0]) {
					bWitnessSigFound = true;
					break;
				}
			}
			if(!bWitnessSigFound)
			{
				errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5003 - " + _("Witness signature not found");
				return error(errorMessage.c_str());
			}
		}
	}
	CRecipient fee;
	string retError = "";
	if(fJustCheck)
	{
		if(!IsValidAliasName(vvchArgs[0]))
		{
			errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5004 - " + _("Alias name does not follow the domain name specification");
			return error(errorMessage.c_str());
		}
		if(theAlias.vchPublicValue.size() > MAX_VALUE_LENGTH)
		{
			errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5005 - " + _("Alias public value too big");
			return error(errorMessage.c_str());
		}
		if(theAlias.vchEncryptionPrivateKey.size() > MAX_ENCRYPTED_GUID_LENGTH)
		{
			errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5006 - " + _("Encryption private key too long");
			return error(errorMessage.c_str());
		}
		if(theAlias.vchEncryptionPublicKey.size() > MAX_ENCRYPTED_GUID_LENGTH)
		{
			errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5007 - " + _("Encryption public key too long");
			return error(errorMessage.c_str());
		}
		switch (op) {
			case OP_ALIAS_ACTIVATE:
				if (prevOp != OP_ALIAS_ACTIVATE)
				{
					errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5008 - " + _("Alias input to this transaction not found");
					return error(errorMessage.c_str());
				}
				// Check new/activate hash
				if (vvchPrevArgs.size() <= 0 || vvchPrevArgs[0] != vchHash)
				{
					errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5009 - " + _("Alias new and activate hash mismatch");
					return error(errorMessage.c_str());
				}
				if (vvchArgs.size() <= 1 || theAlias.vchGUID != vvchArgs[1])
				{
					errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5010 - " + _("Alias input guid mismatch");
					return error(errorMessage.c_str());
				}
				if(theAlias.vchAddress.empty())
				{
					errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5011 - " + _("Alias address cannot be empty");
					return error(errorMessage.c_str());
				}
				break;
			case OP_ALIAS_UPDATE:
				if (!IsAliasOp(prevOp))
				{
					errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5012 - " + _("Alias input to this transaction not found");
					return error(errorMessage.c_str());
				}
				if (!theAlias.IsNull())
				{
					if (theAlias.vchAlias != vvchArgs[0])
					{
						errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5013 - " + _("Guid in data output doesn't match guid in transaction");
						return error(errorMessage.c_str());
					}
					if (vvchArgs.size() <= 1 || theAlias.vchGUID != vvchArgs[1])
					{
						errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5014 - " + _("Alias input guid mismatch");
						return error(errorMessage.c_str());
					}
				}
				break;
		default:
				errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5015 - " + _("Alias transaction has unknown op");
				return error(errorMessage.c_str());
		}

	}
	if (!fJustCheck) {
		CAliasIndex dbAlias;
		string strName = stringFromVch(vvchArgs[0]);
		boost::algorithm::to_lower(strName);
		vchAlias = vchFromString(strName);
		// get the alias from the DB
		if (!GetAlias(vchAlias, dbAlias))
		{
			if (op == OP_ALIAS_UPDATE)
			{
				errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5016 - " + _("Failed to read from alias DB");
				return true;
			}
		}
		// whitelist alias updates don't update expiry date
		if (!vchData.empty() && theAlias.offerWhitelist.entries.empty() && theAlias.nExpireTime > 0)
		{
			CAmount fee = GetDataFee(tx.vout[nDataOut].scriptPubKey);
			float fYears;
			//  get expire time and figure out if alias payload pays enough fees for expiry
			int nHeightTmp = nHeight;
			if (nHeightTmp > chainActive.Height())
				nHeightTmp = chainActive.Height();
			uint64_t nTimeExpiry = theAlias.nExpireTime - chainActive[nHeightTmp]->GetMedianTimePast();
			// ensure aliases are good for atleast an hour
			if (nTimeExpiry < 3600) {
				nTimeExpiry = 3600;
				theAlias.nExpireTime = chainActive[nHeightTmp]->GetMedianTimePast() + 3600;
			}
			fYears = nTimeExpiry / ONE_YEAR_IN_SECONDS;
			if (fYears < 1)
				fYears = 1;
			fee *= powf(2.88, fYears);

			if ((fee - 10000) > tx.vout[nDataOut].nValue)
			{
				errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5017 - " + _("Transaction does not pay enough fee: ") + ValueFromAmount(tx.vout[nDataOut].nValue).write() + "/" + ValueFromAmount(fee - 10000).write() + "/" + boost::lexical_cast<string>(fYears) + " years.";
				return true;
			}
		}
		bool theAliasNull = theAlias.IsNull();
		string strResponseEnglish = "";
		string strResponseGUID = "";
		string strResponse = GetSyscoinTransactionDescription(tx, op, strResponseEnglish, ALIAS, strResponseGUID);
		const string &user1 = stringFromVch(vvchArgs[0]);
		string user2 = "";
		string user3 = "";
		if (!theAlias.vchAddress.empty())
			user2 = EncodeBase58(theAlias.vchAddress);
		if (op == OP_ALIAS_UPDATE)
		{
			CTxDestination aliasDest;
			if (vvchPrevArgs.size() <= 0 || vvchPrevArgs[0] != vvchArgs[0] || vvchPrevArgs[1] != vvchArgs[1] || prevCoins.IsSpent() || !ExtractDestination(prevCoins.out.scriptPubKey, aliasDest))
			{
				errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5018 - " + _("Cannot extract destination of alias input");
				if (!theAliasNull)
					theAlias = dbAlias;
			}
			else
			{
				CSyscoinAddress prevaddy(aliasDest);
				if (EncodeBase58(dbAlias.vchAddress) != prevaddy.ToString())
				{
					errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5019 - " + _("You are not the owner of this alias");
					if (!theAliasNull)
						theAlias = dbAlias;
				}
				else
					bDestCheckFailed = false;

			}

			if (dbAlias.vchGUID != vvchArgs[1] || dbAlias.vchAlias != vvchArgs[0])
			{
				errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5020 - " + _("Cannot edit this alias, guid mismatch");
				if (!theAliasNull)
					theAlias = dbAlias;

			}
			if (!theAliasNull) {
				COfferLinkWhitelist whiteList;
				// if updating whitelist, we dont allow updating any alias details
				if (theAlias.offerWhitelist.entries.size() > 0)
				{
					whiteList = theAlias.offerWhitelist;
					theAlias = dbAlias;
				}
				else
				{
					// can't edit whitelist through aliasupdate
					theAlias.offerWhitelist = dbAlias.offerWhitelist;
					if (theAlias.vchPublicValue.empty())
						theAlias.vchPublicValue = dbAlias.vchPublicValue;
					if (theAlias.vchEncryptionPrivateKey.empty())
						theAlias.vchEncryptionPrivateKey = dbAlias.vchEncryptionPrivateKey;
					if (theAlias.vchEncryptionPublicKey.empty())
						theAlias.vchEncryptionPublicKey = dbAlias.vchEncryptionPublicKey;
					if (theAlias.nExpireTime == 0)
						theAlias.nExpireTime = dbAlias.nExpireTime;
					if (theAlias.vchAddress.empty())
						theAlias.vchAddress = dbAlias.vchAddress;

					theAlias.vchGUID = dbAlias.vchGUID;
					theAlias.vchAlias = dbAlias.vchAlias;
					// if transfer
					if (dbAlias.vchAddress != theAlias.vchAddress)
					{
						// make sure xfer to pubkey doesn't point to an alias already, otherwise don't assign pubkey to alias
						// we want to avoid aliases with duplicate addresses
						if (paliasdb->ExistsAddress(theAlias.vchAddress))
						{
							vector<unsigned char> vchMyAlias;
							if (paliasdb->ReadAddress(theAlias.vchAddress, vchMyAlias) && !vchMyAlias.empty() && vchMyAlias != dbAlias.vchAlias)
							{
								CAliasIndex dbReadAlias;
								// ensure that you block transferring only if the recv address has an active alias associated with it
								if (GetAlias(vchMyAlias, dbReadAlias)) {
									errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5021 - " + _("An alias already exists with that address, try another public key");
									theAlias = dbAlias;
								}
							}
						}
						if (dbAlias.nAccessFlags < 2)
						{
							errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5022 - " + _("Cannot edit this alias. Insufficient privileges.");
							theAlias = dbAlias;
						}
						// let old address be re-occupied by a new alias
						if (!bSanityCheck && errorMessage.empty())
						{
							paliasdb->EraseAddress(dbAlias.vchAddress);
						}
					}
					else
					{
						if (dbAlias.nAccessFlags < 1)
						{
							errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5023 - " + _("Cannot edit this alias. It is view-only.");
							theAlias = dbAlias;
						}
					}
					if (theAlias.nAccessFlags > dbAlias.nAccessFlags)
					{
						errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5024 - " + _("Cannot modify for more lenient access. Only tighter access level can be granted.");
						theAlias = dbAlias;
					}
				}


				// if the txn whitelist entry exists (meaning we want to remove or add)
				if (whiteList.entries.size() >= 1)
				{
					if (whiteList.entries.size() > 20)
					{
						errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5025 -" + _("Too many affiliates for this whitelist, maximum 20 entries allowed");
						theAlias.offerWhitelist.SetNull();
					}
					// special case we use to remove all entries
					else if (whiteList.entries.size() == 1 && whiteList.entries.begin()->second.nDiscountPct == 127)
					{
						if (theAlias.offerWhitelist.entries.empty())
						{
							errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5026 - " + _("Whitelist is already empty");
						}
						else
							theAlias.offerWhitelist.SetNull();
					}
					else
					{
						for (auto const &it : whiteList.entries)
						{
							COfferLinkWhitelistEntry entry;
							const COfferLinkWhitelistEntry& newEntry = it.second;
							if (newEntry.nDiscountPct > 99) {
								errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5027 -" + _("Whitelist discount must be between 0 and 99");
								continue;
							}
							// the stored whitelist has this entry (and its the same) then we want to remove this entry
							if (theAlias.offerWhitelist.GetLinkEntryByHash(newEntry.aliasLinkVchRand, entry) && newEntry == entry)
							{
								theAlias.offerWhitelist.RemoveWhitelistEntry(newEntry.aliasLinkVchRand);
							}
							// we want to add it to the whitelist
							else
							{
								if (theAlias.offerWhitelist.entries.size() < 20)
									theAlias.offerWhitelist.PutWhitelistEntry(newEntry);
								else
								{
									errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5028 -" + _("Too many affiliates for this whitelist, maximum 20 entries allowed");
								}
							}
						}
					}
				}
			}
		}
		else if (op == OP_ALIAS_ACTIVATE)
		{
			if (!dbAlias.IsNull())
			{
				errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5029 - " + _("Trying to renew an alias that isn't expired");
				return true;
			}
			if (paliasdb->ExistsAddress(theAlias.vchAddress) && chainActive.Tip()->GetMedianTimePast() < GetAliasExpiration(theAlias))
			{
				errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5030 - " + _("Trying to create an alias with an address of an alias that isn't expired");
				return true;
			}
			bDestCheckFailed = false;
		}
		if (!theAliasNull)
		{
			if (!bSanityCheck) {
				if (strResponse != "") {
					paliasdb->WriteAliasIndexTxHistory(user1, user2, user3, tx.GetHash(), nHeight, strResponseEnglish, strName);
				}
			}
			theAlias.nHeight = nHeight;
			theAlias.txHash = tx.GetHash();

			CAliasUnprunable aliasUnprunable;
			aliasUnprunable.vchGUID = theAlias.vchGUID;
			aliasUnprunable.nExpireTime = theAlias.nExpireTime;
			if (!bSanityCheck) {
				if (!paliasdb->WriteAlias(aliasUnprunable, theAlias.vchAddress, theAlias, op))
				{
					errorMessage = "SYSCOIN_ALIAS_CONSENSUS_ERROR: ERRCODE: 5031 - " + _("Failed to write to alias DB");
					return error(errorMessage.c_str());
				}

				if (fDebug)
					LogPrintf(
						"CONNECTED ALIAS: name=%s  op=%s  hash=%s  height=%d fJustCheck=%d\n",
						strName.c_str(),
						aliasFromOp(op).c_str(),
						tx.GetHash().ToString().c_str(), nHeight, fJustCheck ? 1 : -1);
			}
		}
	}

	return true;
}

string stringFromValue(const UniValue& value) {
	string strName = value.get_str();
	return strName;
}
vector<unsigned char> vchFromValue(const UniValue& value) {
	string strName = value.get_str();
	unsigned char *strbeg = (unsigned char*) strName.c_str();
	return vector<unsigned char>(strbeg, strbeg + strName.size());
}

std::vector<unsigned char> vchFromString(const std::string &str) {
	unsigned char *strbeg = (unsigned char*) str.c_str();
	return vector<unsigned char>(strbeg, strbeg + str.size());
}

string stringFromVch(const vector<unsigned char> &vch) {
	string res;
	vector<unsigned char>::const_iterator vi = vch.begin();
	while (vi != vch.end()) {
		res += (char) (*vi);
		vi++;
	}
	return res;
}
bool GetSyscoinData(const CTransaction &tx, vector<unsigned char> &vchData, vector<unsigned char> &vchHash, int& nOut)
{
	nOut = GetSyscoinDataOutput(tx);
    if(nOut == -1)
	   return false;

	const CScript &scriptPubKey = tx.vout[nOut].scriptPubKey;
	return GetSyscoinData(scriptPubKey, vchData, vchHash);
}
bool IsValidAliasName(const std::vector<unsigned char> &vchAlias)
{
	return (vchAlias.size() <= 71 && vchAlias.size() >= 3);
}
bool GetSyscoinData(const CScript &scriptPubKey, vector<unsigned char> &vchData, vector<unsigned char> &vchHash)
{
	CScript::const_iterator pc = scriptPubKey.begin();
	opcodetype opcode;
	if (!scriptPubKey.GetOp(pc, opcode))
		return false;
	if(opcode != OP_RETURN)
		return false;
	if (!scriptPubKey.GetOp(pc, opcode, vchData))
		return false;
	if (!scriptPubKey.GetOp(pc, opcode, vchHash))
		return false;
	return true;
}
void GetAddress(const CAliasIndex& alias, CSyscoinAddress* address,CScript& script,const uint32_t nPaymentOption)
{
	if(!address)
		return;
	CChainParams::AddressType myAddressType = PaymentOptionToAddressType(nPaymentOption);
	CSyscoinAddress addrTmp = CSyscoinAddress(EncodeBase58(alias.vchAddress));
	address[0] = CSyscoinAddress(addrTmp.Get(), myAddressType);
	script = GetScriptForDestination(address[0].Get());
}
bool CAliasIndex::UnserializeFromData(const vector<unsigned char> &vchData, const vector<unsigned char> &vchHash) {
    try {
        CDataStream dsAlias(vchData, SER_NETWORK, PROTOCOL_VERSION);
        dsAlias >> *this;

		vector<unsigned char> vchAliasData;
		Serialize(vchAliasData);
		const uint256 &calculatedHash = Hash(vchAliasData.begin(), vchAliasData.end());
		const vector<unsigned char> &vchRandAlias = vchFromValue(calculatedHash.GetHex());
		if(vchRandAlias != vchHash)
		{
			SetNull();
			return false;
		}
    } catch (std::exception &e) {
		SetNull();
        return false;
    }
	return true;
}
bool CAliasIndex::UnserializeFromTx(const CTransaction &tx) {
	vector<unsigned char> vchData;
	vector<unsigned char> vchHash;
	int nOut;
	if(!GetSyscoinData(tx, vchData, vchHash, nOut))
	{
		SetNull();
		return false;
	}
	if(!UnserializeFromData(vchData, vchHash))
	{
		return false;
	}
    return true;
}
void CAliasIndex::Serialize(vector<unsigned char>& vchData) {
    CDataStream dsAlias(SER_NETWORK, PROTOCOL_VERSION);
    dsAlias << *this;
    vchData = vector<unsigned char>(dsAlias.begin(), dsAlias.end());

}

// TODO: need to cleanout CTxOuts (transactions stored on disk) which have data stored in them after expiry, erase at same time on startup so pruning can happen properly
bool CAliasDB::CleanupDatabase(int &servicesCleaned)
{
	boost::scoped_ptr<CDBIterator> pcursor(NewIterator());
	pcursor->SeekToFirst();
	CAliasIndex txPos;
	pair<string, vector<unsigned char> > key;
    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        try {
			if (pcursor->GetKey(key) && key.first == "namei") {
				pcursor->GetValue(txPos);
  				if (chainActive.Tip()->GetMedianTimePast() >= txPos.nExpireTime)
				{
					servicesCleaned++;
					EraseAlias(key.second, true);
				} 
				
            }
			else if (pcursor->GetKey(key) && key.first == "namea") {
				vector<unsigned char> value;
				CAliasIndex alias;
				pcursor->GetValue(value);
				if (GetAlias(value, alias) && chainActive.Tip()->GetMedianTimePast() >= alias.nExpireTime)
				{
					servicesCleaned++;
					EraseAddress(alias.vchAddress);
				}

			}
            pcursor->Next();
        } catch (std::exception &e) {
            return error("%s() : deserialize error", __PRETTY_FUNCTION__);
        }
    }
	return true;
}
void CleanupSyscoinServiceDatabases(int &numServicesCleaned)
{
	if(pofferdb != NULL)
		pofferdb->CleanupDatabase(numServicesCleaned);
	if(pescrowdb!= NULL)
		pescrowdb->CleanupDatabase(numServicesCleaned);
	if(pcertdb!= NULL)
		pcertdb->CleanupDatabase(numServicesCleaned);
	if (paliasdb != NULL) 
		paliasdb->CleanupDatabase(numServicesCleaned);
	
	if(paliasdb != NULL)
	{
		if (!paliasdb->Flush())
			LogPrintf("Failed to write to alias database!");
	}
	if(pofferdb != NULL)
	{
		if (!pofferdb->Flush())
			LogPrintf("Failed to write to offer database!");
	}
	if(pcertdb != NULL)
	{
		if (!pcertdb->Flush())
			LogPrintf("Failed to write to cert database!");
	}
	if(pescrowdb != NULL)
	{
		if (!pescrowdb->Flush())
			LogPrintf("Failed to write to escrow database!");
	}
	if (passetdb != NULL)
	{
		if (!passetdb->Flush())
			LogPrintf("Failed to write to asset database!");
	}
	if (passetallocationdb != NULL)
	{
		if (!passetallocationdb->Flush())
			LogPrintf("Failed to write to asset allocation database!");
	}
}
bool GetAlias(const vector<unsigned char> &vchAlias,
	CAliasIndex& txPos) {
	if (!paliasdb || !paliasdb->ReadAlias(vchAlias, txPos))
		return false;
	
	if (chainActive.Tip()->GetMedianTimePast() >= txPos.nExpireTime) {
		txPos.SetNull();
		return false;
	}
	
	return true;
}
bool GetAddressFromAlias(const std::string& strAlias, std::string& strAddress, std::vector<unsigned char> &vchPubKey) {

	string strLowerAlias = strAlias;
	boost::algorithm::to_lower(strLowerAlias);
	const vector<unsigned char> &vchAlias = vchFromValue(strLowerAlias);

	CAliasIndex alias;
	// check for alias existence in DB
	if (!GetAlias(vchAlias, alias))
		return false;

	strAddress = EncodeBase58(alias.vchAddress);
	vchPubKey = alias.vchEncryptionPublicKey;
	return true;
}
bool GetAliasFromAddress(const std::string& strAddress, std::string& strAlias, std::vector<unsigned char> &vchPubKey) {

	vector<unsigned char> vchAddress;
	DecodeBase58(strAddress, vchAddress);
	if (!paliasdb || !paliasdb->ExistsAddress(vchAddress))
		return false;

	// check for alias address mapping existence in DB
	vector<unsigned char> vchAlias;
	if (!paliasdb->ReadAddress(vchAddress, vchAlias))
		return false;
	if (vchAlias.empty())
		return false;
	
	strAlias = stringFromVch(vchAlias);
	CAliasIndex alias;
	// check for alias existence in DB
	if (!GetAlias(vchAlias, alias))
		return false;
	vchPubKey = alias.vchEncryptionPublicKey;
	return true;
}

bool GetAliasOfTx(const CTransaction& tx, vector<unsigned char>& name) {
	if (tx.nVersion != SYSCOIN_TX_VERSION)
		return false;
	vector<vector<unsigned char> > vvchArgs;
	int op;

	bool good = DecodeAliasTx(tx, op, vvchArgs);
	if (!good)
		return error("GetAliasOfTx() : could not decode a syscoin tx");

	switch (op) {
	case OP_ALIAS_ACTIVATE:
	case OP_ALIAS_UPDATE:
		name = vvchArgs[0];
		return true;
	}
	return false;
}
bool DecodeAndParseSyscoinTx(const CTransaction& tx, int& op,
		vector<vector<unsigned char> >& vvch, char& type)
{
	return  
		DecodeAndParseCertTx(tx, op, vvch, type)
		|| DecodeAndParseOfferTx(tx, op, vvch, type)
		|| DecodeAndParseEscrowTx(tx, op, vvch, type)
		|| DecodeAndParseAssetTx(tx, op, vvch, type)
		|| DecodeAndParseAssetAllocationTx(tx, op, vvch, type)
		|| DecodeAndParseAliasTx(tx, op, vvch, type);
}
bool DecodeAndParseAliasTx(const CTransaction& tx, int& op,
		vector<vector<unsigned char> >& vvch, char &type)
{
	CAliasIndex alias;
	bool decode = DecodeAliasTx(tx, op, vvch);
	if(decode)
	{
		bool parse = alias.UnserializeFromTx(tx);
		if (decode && parse) {
			type = ALIAS;
			return true;
		} 
	}
	return false;
}
bool DecodeAliasTx(const CTransaction& tx, int& op,
		vector<vector<unsigned char> >& vvch) {
	bool found = false;


	// Strict check - bug disallowed
	for (unsigned int i = 0; i < tx.vout.size(); i++) {
		const CTxOut& out = tx.vout[i];
		vector<vector<unsigned char> > vvchRead;
		if (DecodeAliasScript(out.scriptPubKey, op, vvchRead)) {
			found = true;
			vvch = vvchRead;
			break;
		}
	}
	if (!found)
		vvch.clear();

	return found;
}
bool FindAliasInTx(const CTransaction& tx, vector<vector<unsigned char> >& vvch) {
	int op;
	for (unsigned int i = 0; i < tx.vin.size(); i++) {
		Coin prevCoins;
		if (!GetUTXOCoin(tx.vin[i].prevout, prevCoins))
			continue;
		// ensure inputs are unspent when doing consensus check to add to block
		if (DecodeAliasScript(prevCoins.out.scriptPubKey, op, vvch)) {
			return true;
		}
	}
	return false;
}

bool DecodeAliasScript(const CScript& script, int& op,
		vector<vector<unsigned char> > &vvch, CScript::const_iterator& pc) {
	opcodetype opcode;
	vvch.clear();
	if (!script.GetOp(pc, opcode))
		return false;
	if (opcode < OP_1 || opcode > OP_16)
		return false;
	op = CScript::DecodeOP_N(opcode);
	if (op != OP_SYSCOIN_ALIAS)
		return false;
	if (!script.GetOp(pc, opcode))
		return false;
	if (opcode < OP_1 || opcode > OP_16)
		return false;
	op = CScript::DecodeOP_N(opcode);
	if (!IsAliasOp(op))
		return false;
	bool found = false;
	for (;;) {
		vector<unsigned char> vch;
		if (!script.GetOp(pc, opcode, vch))
			return false;
		if (opcode == OP_DROP || opcode == OP_2DROP)
		{
			found = true;
			break;
		}
		if (!(opcode >= 0 && opcode <= OP_PUSHDATA4))
			return false;
		vvch.push_back(vch);
	}

	// move the pc to after any DROP or NOP
	while (opcode == OP_DROP || opcode == OP_2DROP) {
		if (!script.GetOp(pc, opcode))
			break;
	}

	pc--;
	return found;
}
bool DecodeAliasScript(const CScript& script, int& op,
		vector<vector<unsigned char> > &vvch) {
	CScript::const_iterator pc = script.begin();
	return DecodeAliasScript(script, op, vvch, pc);
}
bool RemoveAliasScriptPrefix(const CScript& scriptIn, CScript& scriptOut) {
	int op;
	vector<vector<unsigned char> > vvch;
	CScript::const_iterator pc = scriptIn.begin();

	if (!DecodeAliasScript(scriptIn, op, vvch, pc))
		return false;
	scriptOut = CScript(pc, scriptIn.end());
	return true;
}
void CreateAliasRecipient(const CScript& scriptPubKey, CRecipient& recipient)
{
	CRecipient recp = { scriptPubKey, recipient.nAmount, false };
	recipient = recp;
	CAmount nFee = CWallet::GetMinimumFee(3000, nTxConfirmTarget, mempool);
	recipient.nAmount = nFee;
}
void CreateRecipient(const CScript& scriptPubKey, CRecipient& recipient)
{
	CRecipient recp = {scriptPubKey, recipient.nAmount, false};
	recipient = recp;
	CTxOut txout(recipient.nAmount, scriptPubKey);
	size_t nSize = GetSerializeSize(txout, SER_DISK, 0) + 148u;
	recipient.nAmount = 3 * minRelayTxFee.GetFee(nSize);
}
void CreateFeeRecipient(CScript& scriptPubKey, const vector<unsigned char>& data, CRecipient& recipient)
{
	CAmount nFee = 0;
	// add hash to data output (must match hash in inputs check with the tx scriptpubkey hash)
    uint256 hash = Hash(data.begin(), data.end());
    vector<unsigned char> vchHashRand = vchFromValue(hash.GetHex());
	scriptPubKey << vchHashRand;
	CRecipient recp = {scriptPubKey, 0, false};
	recipient = recp;
}
CAmount GetDataFee(const CScript& scriptPubKey)
{
	CAmount nFee = 0;
	CRecipient recp = {scriptPubKey, 0, false};
	CTxOut txout(0, scriptPubKey);
    size_t nSize = GetSerializeSize(txout, SER_DISK,0)+148u;
	nFee = CWallet::GetMinimumFee(nSize, nTxConfirmTarget, mempool);
	recp.nAmount = nFee;
	return recp.nAmount;
}
bool CheckParam(const UniValue& params, const unsigned int index)
{
	if(params.size() > index)
	{
		if(params[index].isStr())
		{
			if( params[index].get_str().size() > 0 && params[index].get_str() != "\"\"")
				return true;
		}
		else if(params[index].isArray())
			return params[index].get_array().size() > 0;
	}
	return false;
}

void CAliasDB::WriteAliasIndex(const CAliasIndex& alias, const int &op) {
	UniValue oName(UniValue::VOBJ);
	oName.push_back(Pair("_id", stringFromVch(alias.vchAlias)));
	CSyscoinAddress address(EncodeBase58(alias.vchAddress));
	oName.push_back(Pair("address", address.ToString()));
	oName.push_back(Pair("expires_on", alias.nExpireTime));
	GetMainSignals().NotifySyscoinUpdate(oName.write().c_str(), "alias");
	WriteAliasIndexHistory(alias, op);
}
void CAliasDB::WriteAliasIndexHistory(const CAliasIndex& alias, const int &op) {
	UniValue oName(UniValue::VOBJ);
	BuildAliasIndexerHistoryJson(alias, oName);
	oName.push_back(Pair("op", aliasFromOp(op)));
	GetMainSignals().NotifySyscoinUpdate(oName.write().c_str(), "aliashistory");
}
bool BuildAliasIndexerTxHistoryJson(const string &user1, const string &user2, const string &user3, const uint256 &txHash, const unsigned int& nHeight, const string &type, const string &guid, UniValue& oName)
{
	oName.push_back(Pair("_id", txHash.GetHex()+"-"+guid));
	oName.push_back(Pair("user1", user1));
	oName.push_back(Pair("user2", user2));
	oName.push_back(Pair("user3", user3));
	oName.push_back(Pair("type", type));
	oName.push_back(Pair("height", (int)nHeight));
	int64_t nTime = 0;
	if (chainActive.Height() >= nHeight) {
		CBlockIndex *pindex = chainActive[nHeight];
		if (pindex) {
			nTime = pindex->GetMedianTimePast();
		}
	}
	oName.push_back(Pair("time", nTime));
	return true;
}
void CAliasDB::WriteAliasIndexTxHistory(const string &user1, const string &user2, const string &user3, const uint256 &txHash, const unsigned int& nHeight, const string &type, const string &guid) {
	UniValue oName(UniValue::VOBJ);
	BuildAliasIndexerTxHistoryJson(user1, user2, user3, txHash, nHeight, type, guid, oName);
	GetMainSignals().NotifySyscoinUpdate(oName.write().c_str(), "aliastxhistory");
}
UniValue SyscoinListReceived(bool includeempty=true)
{
	if (!pwalletMain)
		return NullUniValue;
	map<string, int> mapAddress;
	UniValue ret(UniValue::VARR);
	BOOST_FOREACH(const PAIRTYPE(CSyscoinAddress, CAddressBookData)& item, pwalletMain->mapAddressBook)
	{
		const CSyscoinAddress& address = item.first;
		const string& strAccount = item.second.name;

		isminefilter filter = ISMINE_SPENDABLE;
		isminefilter mine = IsMine(*pwalletMain, address.Get());
		if (!(mine & filter))
			continue;
		const string& strAddress = address.ToString();

		vector<unsigned char> vchMyAlias;
		vector<unsigned char> vchAddress;
		DecodeBase58(strAddress, vchAddress);
		paliasdb->ReadAddress(vchAddress, vchMyAlias);
		
		UniValue paramsBalance(UniValue::VARR);
		UniValue param(UniValue::VOBJ);
		UniValue balanceParams(UniValue::VARR);
		balanceParams.push_back(strAddress);
		param.push_back(Pair("addresses", balanceParams));
		paramsBalance.push_back(param);
		JSONRPCRequest request;
		request.params = paramsBalance;
		const UniValue &resBalance = getaddressbalance(request);
		UniValue obj(UniValue::VOBJ);
		obj.push_back(Pair("address", strAddress));
		const CAmount& nBalance = AmountFromValue(find_value(resBalance.get_obj(), "balance"));
		if (includeempty || (!includeempty && nBalance > 0)) {
			obj.push_back(Pair("balance", ValueFromAmount(nBalance)));
			obj.push_back(Pair("label", strAccount));
			obj.push_back(Pair("alias", stringFromVch(vchMyAlias)));
			ret.push_back(obj);
		}
		mapAddress[strAddress] = 1;
	}

	vector<COutput> vecOutputs;
	pwalletMain->AvailableCoins(vecOutputs, false, NULL, includeempty, ALL_COINS, false, true);
	BOOST_FOREACH(const COutput& out, vecOutputs) {
		CTxDestination address;
		if (!ExtractDestination(out.tx->tx->vout[out.i].scriptPubKey, address))
			continue;

		CSyscoinAddress sysAddress(address);
		const string& strAddress = sysAddress.ToString();

		if (mapAddress.find(strAddress) != mapAddress.end())
			continue;


		vector<unsigned char> vchMyAlias;
		vector<unsigned char> vchAddress;
		DecodeBase58(strAddress, vchAddress);
		paliasdb->ReadAddress(vchAddress, vchMyAlias);
	
		UniValue paramsBalance(UniValue::VARR);
		UniValue param(UniValue::VOBJ);
		UniValue balanceParams(UniValue::VARR);
		balanceParams.push_back(strAddress);
		param.push_back(Pair("addresses", balanceParams));
		paramsBalance.push_back(param);
		JSONRPCRequest request;
		request.params = paramsBalance;
		const UniValue &resBalance = getaddressbalance(request);
		UniValue obj(UniValue::VOBJ);
		obj.push_back(Pair("address", strAddress));
		const CAmount& nBalance = AmountFromValue(find_value(resBalance.get_obj(), "balance"));
		if (includeempty || (!includeempty && nBalance > 0)) {
			obj.push_back(Pair("balance", ValueFromAmount(nBalance)));
			obj.push_back(Pair("label", ""));
			obj.push_back(Pair("alias", stringFromVch(vchMyAlias)));
			ret.push_back(obj);
		}
		mapAddress[strAddress] = 1;

	}
	return ret;
}
UniValue syscointxfund_helper(const vector<unsigned char> &vchAlias, const vector<unsigned char> &vchWitness, const CRecipient &aliasRecipient, vector<CRecipient> &vecSend, bool transferAlias) {
	CMutableTransaction txNew;
	txNew.nVersion = SYSCOIN_TX_VERSION;
	if (!vchWitness.empty())
	{
		COutPoint aliasOutPointWitness;
		aliasunspent(vchWitness, aliasOutPointWitness);
		if (aliasOutPointWitness.IsNull())
		{
			throw runtime_error("SYSCOIN_RPC_ERROR ERRCODE: 9000 - " + _("This transaction requires a witness but not enough outputs found for witness alias: ") + stringFromVch(vchWitness));
		}
		Coin pcoinW;
		if (GetUTXOCoin(aliasOutPointWitness, pcoinW))
			txNew.vin.push_back(CTxIn(aliasOutPointWitness, pcoinW.out.scriptPubKey));
	}

	COutPoint aliasOutPoint;
	unsigned int unspentcount = aliasunspent(vchAlias, aliasOutPoint);
	// for the alias utxo (1 per transaction is used)
	if (unspentcount <= 1 || transferAlias)
	{
		unsigned int iterations = MAX_ALIAS_UPDATES_PER_BLOCK;
		if (transferAlias)
			iterations = 1;
		for (unsigned int i = 0; i < iterations; i++)
			vecSend.push_back(aliasRecipient);
	}
	Coin pcoin;
	if (GetUTXOCoin(aliasOutPoint, pcoin))
		txNew.vin.push_back(CTxIn(aliasOutPoint, pcoin.out.scriptPubKey));

	// set an address for syscointxfund so it uses that address to fund (alias passed in)

	CAliasIndex alias;
	if (!GetAlias(vchAlias, alias))
		throw runtime_error("SYSCOIN_RPC_ERROR ERRCODE: 9000 - " + _("Cannot find alias used to fund this transaction: ") + stringFromVch(vchAlias));
	string strAddress = EncodeBase58(alias.vchAddress);
	

	// vouts to the payees
	for (const auto& recipient : vecSend)
	{
		CTxOut txout(recipient.nAmount, recipient.scriptPubKey);
		if (!txout.IsDust(dustRelayFee))
		{
			txNew.vout.push_back(txout);
		}
	}
	UniValue paramObj(UniValue::VOBJ);
	UniValue paramArr(UniValue::VARR);
	paramArr.push_back(strAddress);
	paramObj.push_back(Pair("addresses", paramArr));


	UniValue paramsFund(UniValue::VARR);
	paramsFund.push_back(EncodeHexTx(txNew));
	paramsFund.push_back(paramObj);
	paramsFund.push_back(transferAlias);
	
	JSONRPCRequest request;
	request.params = paramsFund;
	return syscointxfund(request);
}
UniValue syscointxfund(const JSONRPCRequest& request) {
	const UniValue &params = request.params;
	if (request.fHelp || 1 > params.size() || 3 < params.size())
		throw runtime_error(
			"syscointxfund\n"
			"\nFunds a new syscoin transaction with inputs used from wallet or an array of addresses specified.\n"
			"\nArguments:\n"
			"  \"hexstring\" (string, required) The raw syscoin transaction output given from rpc (ie: aliasnew, aliasupdate)\n"
			"  \"addresses (object, optional) \"\n"
			"    [\n"
			"      \"address\"  (array, string) Address used to fund this transaction. Leave empty to use wallet. Last address gets sent the change.\n"
			"      ,...\n"
			"    ]\n"
			"   \"sendall\" (boolean, optional) If addresses were specified, send all funds found in those addresses.\n"
			"}\n"
			"\nExamples:\n"
			+ HelpExampleCli("syscointxfund", " <hexstring> '{\"addresses\": [\"175tWpb8K1S7NmH4Zx6rewF9WQrcZv245W\"]}' true")
			+ HelpExampleRpc("syscointxfund", " <hexstring> {\"addresses\": [\"175tWpb8K1S7NmH4Zx6rewF9WQrcZv245W\"]} false")
			+ HelpRequiringPassphrase());
	
	
	const string &hexstring = params[0].get_str();
	CMutableTransaction tx;
	if (!DecodeHexTx(tx, hexstring) || tx.nVersion != SYSCOIN_TX_VERSION)
		throw runtime_error("SYSCOIN_ALIAS_RPC_ERROR: ERRCODE: 5500 - " + _("Could not send raw transaction: Cannot decode transaction from hex string"));
	CTransaction txIn(tx);
	// if addresses are passed in use those, otherwise use whatever is in the wallet
	UniValue addresses(UniValue::VOBJ);
	if(params.size() > 1)
		addresses = params[1].get_obj();
	else {
		EnsureWalletIsUnlocked();
		UniValue addressArray(UniValue::VARR);
		UniValue receivedList = SyscoinListReceived(false);
		UniValue recevedListArray = receivedList.get_array();
		for (unsigned int idx = 0; idx < recevedListArray.size(); idx++) {
			if(find_value(recevedListArray[idx].get_obj(), "alias").get_str().empty())
				addressArray.push_back(find_value(recevedListArray[idx].get_obj(), "address").get_str());
		}
		addresses.push_back(Pair("addresses", addressArray));
	}
	bool bSendAll = false;
	if (params.size() > 2)
		bSendAll = params[2].get_bool();
	UniValue paramsUTXO(UniValue::VARR);
	paramsUTXO.push_back(addresses);
	JSONRPCRequest request1;
	request1.params = paramsUTXO;
	const UniValue &resUTXOs = getaddressutxos(request1);
	UniValue utxoArray(UniValue::VARR);
	if (resUTXOs.isArray())
		utxoArray = resUTXOs.get_array();
	else
		throw runtime_error("SYSCOIN_ALIAS_RPC_ERROR: ERRCODE: 5501 - " + _("No funds found in addresses provided"));

	// add total output amount of transaction to desired amount
	CAmount nDesiredAmount = txIn.GetValueOut();
	CAmount nCurrentAmount = 0;
	{
		LOCK(cs_main);
		CCoinsViewCache view(pcoinsTip);
		// get value of inputs
		nCurrentAmount = view.GetValueIn(txIn);
	}
	int op, aliasOp;
	vector<vector<unsigned char> > vvch;
	vector<vector<unsigned char> > vvchAlias;
	if (!DecodeAliasTx(tx, op, vvchAlias))
	{
		FindAliasInTx(tx, vvchAlias);
		// it is assumed if no alias output is found, then it is for another service so this would be an alias update
		op = OP_ALIAS_UPDATE;

	}
	int nHeightTip = chainActive.Height();
	if (nCurrentAmount < nDesiredAmount || bSendAll) {
		const unsigned int nBytes = ::GetSerializeSize(txIn, SER_NETWORK, PROTOCOL_VERSION);
		// min fee based on bytes + 1 change output
		const CAmount &outputFee = CWallet::GetMinimumFee(200u, nTxConfirmTarget, mempool);
		CAmount nFees = CWallet::GetMinimumFee(nBytes, nTxConfirmTarget, mempool) + outputFee;
		// only look for alias inputs if addresses were passed in, if looking through wallet we do not want to fund via alias inputs as we may end up spending alias inputs inadvertently
		if (params.size() > 1) {
			LOCK(mempool.cs);
			// fund with alias inputs first
			for (unsigned int i = 0; i < utxoArray.size(); i++)
			{
				const UniValue& utxoObj = utxoArray[i].get_obj();
				const string &strTxid = find_value(utxoObj, "txid").get_str();
				const uint256& txid = uint256S(strTxid);
				const int& nOut = find_value(utxoObj, "outputIndex").get_int();
				const std::vector<unsigned char> &data(ParseHex(find_value(utxoObj, "script").get_str()));
				const CScript& scriptPubKey = CScript(data.begin(), data.end());
				const CAmount &nValue = find_value(utxoObj, "satoshis").get_int64();
				const CTxIn txIn(txid, nOut, scriptPubKey);
				const COutPoint outPoint(txid, nOut);
				if (std::find(tx.vin.begin(), tx.vin.end(), txIn) != tx.vin.end())
					continue;
				// look for alias inputs only, if not selecting all
				if ((DecodeAliasScript(scriptPubKey, aliasOp, vvch) && vvch.size() > 1 && vvch[0] == vvchAlias[0] && vvch[1] == vvchAlias[1]) || bSendAll) {
					
					if (mempool.mapNextTx.find(outPoint) != mempool.mapNextTx.end())
						continue;
					
					if (pwalletMain && pwalletMain->IsLockedCoin(txid, nOut))
						continue;
					if (!IsOutpointMature(outPoint))
						continue;
					// add 200 bytes of fees to account for every input added to this transaction
					nFees += outputFee;
					tx.vin.push_back(txIn);
					nCurrentAmount += nValue;
					if (nCurrentAmount >= (nDesiredAmount + nFees)) {
						if (!bSendAll)
							break;
					}
				}
			}
		}
		// if after selecting alias inputs we are still not funded, we need to select alias balances to fund this transaction
		if (nCurrentAmount < (nDesiredAmount + nFees)) {
			LOCK(mempool.cs);
			for (unsigned int i = 0; i < utxoArray.size(); i++)
			{
				const UniValue& utxoObj = utxoArray[i].get_obj();
				const string &strTxid = find_value(utxoObj, "txid").get_str();
				const uint256& txid = uint256S(strTxid);
				const int& nOut = find_value(utxoObj, "outputIndex").get_int();
				const std::vector<unsigned char> &data(ParseHex(find_value(utxoObj, "script").get_str()));
				const CScript& scriptPubKey = CScript(data.begin(), data.end());
				const CAmount &nValue = find_value(utxoObj, "satoshis").get_int64();
				const CTxIn txIn(txid, nOut, scriptPubKey);
				const COutPoint outPoint(txid, nOut);
				if (std::find(tx.vin.begin(), tx.vin.end(), txIn) != tx.vin.end())
					continue;
				// look for non alias inputs
				if (DecodeAliasScript(scriptPubKey, aliasOp, vvch))
					continue;
				if (mempool.mapNextTx.find(outPoint) != mempool.mapNextTx.end())
					continue;
				
				if (pwalletMain && pwalletMain->IsLockedCoin(txid, nOut))
					continue;
				if (!IsOutpointMature(outPoint))
					continue;
				// add 200 bytes of fees to account for every input added to this transaction
				nFees += outputFee;
				tx.vin.push_back(txIn);
				nCurrentAmount += nValue;
				if (nCurrentAmount >= (nDesiredAmount + nFees)) {
					if (!bSendAll)
						break;
				}
			}
		}
		const CAmount &nChange = nCurrentAmount - nDesiredAmount - nFees;
		if(nChange < 0)
			throw runtime_error("SYSCOIN_ALIAS_RPC_ERROR: ERRCODE: 5502 - " + _("Insufficient funds for alias creation transaction"));
		// if addresses were passed in, send change back to the last address as policy
		if (params.size() > 1) {
			tx.vout.push_back(CTxOut(nChange, tx.vin.back().scriptSig));
		}
		// else create new change address in this wallet
		else {
			CReserveKey reservekey(pwalletMain);
			CPubKey vchPubKey;
			reservekey.GetReservedKey(vchPubKey, true);
			tx.vout.push_back(CTxOut(nChange, GetScriptForDestination(vchPubKey.GetID())));
		}
	}
	bool fJustCheck = true;
	string errorMessage = "";
	bool bCheckDestError = false;
	sorted_vector<CAssetAllocationTuple> revertedAssetAllocations;
	sorted_vector<vector<unsigned char> > revertedOffers;
	sorted_vector<vector<unsigned char> > revertedCerts;
	CheckAliasInputs(tx, op, vvchAlias, fJustCheck, chainActive.Tip()->nHeight, errorMessage, bCheckDestError, true);
	if (!errorMessage.empty())
		throw runtime_error(errorMessage.c_str());
	CheckAliasInputs(tx, op, vvchAlias, !fJustCheck, chainActive.Tip()->nHeight + 1, errorMessage, bCheckDestError, true);
	if (!errorMessage.empty())
		throw runtime_error(errorMessage.c_str());
	if(bCheckDestError)
		throw runtime_error("SYSCOIN_RPC_ERROR ERRCODE: 9002 - " + _("Alias input is from the wrong address"));
	if (DecodeCertTx(tx, op, vvch))
	{
		CheckCertInputs(tx, op, vvch, vvchAlias[0], fJustCheck, chainActive.Tip()->nHeight, revertedCerts, errorMessage, true);
		if (!errorMessage.empty())
			throw runtime_error(errorMessage.c_str());
		CheckCertInputs(tx, op, vvch, vvchAlias[0], !fJustCheck, chainActive.Tip()->nHeight + 1, revertedCerts, errorMessage, true);
		if (!errorMessage.empty())
			throw runtime_error(errorMessage.c_str());
	}
	if (DecodeAssetTx(tx, op, vvch))
	{
		CheckAssetInputs(tx, op, vvch, vvchAlias[0], fJustCheck, chainActive.Tip()->nHeight, revertedAssetAllocations, errorMessage, true);
		if (!errorMessage.empty())
			throw runtime_error(errorMessage.c_str());
		CheckAssetInputs(tx, op, vvch, vvchAlias[0], !fJustCheck, chainActive.Tip()->nHeight + 1, revertedAssetAllocations, errorMessage, true);
		if (!errorMessage.empty())
			throw runtime_error(errorMessage.c_str());
	}
	if (DecodeAssetAllocationTx(tx, op, vvch))
	{
		CheckAssetAllocationInputs(tx, op, vvch, vvchAlias[0], fJustCheck, chainActive.Tip()->nHeight, revertedAssetAllocations, errorMessage, true);
		if (!errorMessage.empty())
			throw runtime_error(errorMessage.c_str());
		CheckAssetAllocationInputs(tx, op, vvch, vvchAlias[0], !fJustCheck, chainActive.Tip()->nHeight + 1, revertedAssetAllocations, errorMessage, true);
		if (!errorMessage.empty())
			throw runtime_error(errorMessage.c_str());
	}
	if (DecodeEscrowTx(tx, op, vvch))
	{
		CheckEscrowInputs(tx, op, vvch, vvchAlias, fJustCheck, chainActive.Tip()->nHeight, errorMessage, true);
		if (!errorMessage.empty())
			throw runtime_error(errorMessage.c_str());
		CheckEscrowInputs(tx, op, vvch, vvchAlias, !fJustCheck, chainActive.Tip()->nHeight + 1, errorMessage, true);
		if (!errorMessage.empty())
			throw runtime_error(errorMessage.c_str());
	}
	if (DecodeOfferTx(tx, op, vvch))
	{
		CheckOfferInputs(tx, op, vvch, vvchAlias[0], fJustCheck, chainActive.Tip()->nHeight, revertedOffers, errorMessage, true);
		if (!errorMessage.empty())
			throw runtime_error(errorMessage.c_str());
		CheckOfferInputs(tx, op, vvch, vvchAlias[0], !fJustCheck, chainActive.Tip()->nHeight + 1, revertedOffers, errorMessage, true);
		if (!errorMessage.empty())
			throw runtime_error(errorMessage.c_str());

	}

	// pass back new raw transaction
	UniValue res(UniValue::VARR);
	res.push_back(EncodeHexTx(tx));
	return res;
}

UniValue aliasnew(const JSONRPCRequest& request) {
	const UniValue &params = request.params;
	if (request.fHelp || 8 != params.size())
		throw runtime_error(
			"aliasnew [aliasname] [public value] [accept_transfers_flags=3] [expire_timestamp] [address] [encryption_privatekey] [encryption_publickey] [witness]\n"
						"<aliasname> alias name.\n"
						"<public value> alias public profile data, 256 characters max.\n"
						"<accept_transfers_flags> 0 for none, 1 for accepting certificate transfers, 2 for accepting asset transfers and 3 for all. Default is 3.\n"	
						"<expire_timestamp> Epoch time when to expire alias. It is exponentially more expensive per year, calculation is FEERATE*(2.88^years). FEERATE is the dynamic satoshi per byte fee set in the rate peg alias used for this alias. Defaults to 1 hour.\n"	
						"<address> Address for this alias.\n"		
						"<encryption_privatekey> Encrypted private key used for encryption/decryption of private data related to this alias. Should be encrypted to publickey.\n"
						"<encryption_publickey> Public key used for encryption/decryption of private data related to this alias.\n"						
						"<witness> Witness alias name that will sign for web-of-trust notarization of this transaction.\n"							
						+ HelpRequiringPassphrase());
	vector<unsigned char> vchAlias = vchFromString(params[0].get_str());
	string strName = stringFromVch(vchAlias);
	/*Above pattern makes sure domain name matches the following criteria :

	The domain name should be a-z | 0-9 and hyphen(-)
	The domain name should between 3 and 63 characters long
	Last Tld can be 2 to a maximum of 6 characters
	The domain name should not start or end with hyphen (-) (e.g. -syscoin.org or syscoin-.org)
	The domain name can be a subdomain (e.g. sys.blogspot.com)*/

	using namespace boost::xpressive;
	using namespace boost::algorithm;
	to_lower(strName);
	smatch nameparts;
	sregex domainwithtldregex = sregex::compile("^((?!-)[a-z0-9-]{3,64}(?<!-)\\.)+[a-z]{2,6}$");
	sregex domainwithouttldregex = sregex::compile("^((?!-)[a-z0-9-]{3,64}(?<!-))");

	if (find_first(strName, "."))
	{
		if (!regex_search(strName, nameparts, domainwithtldregex) || string(nameparts[0]) != strName)
			throw runtime_error("SYSCOIN_ALIAS_RPC_ERROR: ERRCODE: 5504 - " + _("Invalid Syscoin Identity. Must follow the domain name spec of 3 to 64 characters with no preceding or trailing dashes and a TLD of 2 to 6 characters"));
	}
	else
	{
		if (!regex_search(strName, nameparts, domainwithouttldregex) || string(nameparts[0]) != strName)
			throw runtime_error("SYSCOIN_ALIAS_RPC_ERROR: ERRCODE: 5505 - " + _("Invalid Syscoin Identity. Must follow the domain name spec of 3 to 64 characters with no preceding or trailing dashes"));
	}



	vchAlias = vchFromString(strName);

	vector<unsigned char> vchPublicValue;
	string strPublicValue = "";
	strPublicValue = params[1].get_str();
	vchPublicValue = vchFromString(strPublicValue);

	unsigned char nAcceptTransferFlags = 3;
	nAcceptTransferFlags = params[2].get_int();
	uint64_t nTime = 0;
	nTime = params[3].get_int64();
	// sanity check set to 1 hr
	if (nTime < chainActive.Tip()->GetMedianTimePast() + 3600)
		nTime = chainActive.Tip()->GetMedianTimePast() + 3600;

	string strAddress = "";
	strAddress = params[4].get_str();

	string strEncryptionPrivateKey = "";
	strEncryptionPrivateKey = params[5].get_str();
	string strEncryptionPublicKey = "";
	strEncryptionPublicKey = params[6].get_str();
	vector<unsigned char> vchWitness;
	vchWitness = vchFromValue(params[7]);

	CMutableTransaction tx;
	tx.nVersion = SYSCOIN_TX_VERSION;
	tx.vin.clear();
	tx.vout.clear();
	CAliasIndex oldAlias;
	if (GetAlias(vchAlias, oldAlias))
		throw runtime_error("SYSCOIN_ALIAS_RPC_ERROR: ERRCODE: 5506 - " + _("This alias already exists"));


	const vector<unsigned char> &vchRandAlias = vchFromString(GenerateSyscoinGuid());

	// build alias
	CAliasIndex newAlias, newAlias1;
	newAlias.vchGUID = vchRandAlias;
	newAlias.vchAlias = vchAlias;
	if (!strEncryptionPublicKey.empty())
		newAlias.vchEncryptionPublicKey = ParseHex(strEncryptionPublicKey);
	if (!strEncryptionPrivateKey.empty())
		newAlias.vchEncryptionPrivateKey = ParseHex(strEncryptionPrivateKey);
	newAlias.vchPublicValue = vchPublicValue;
	newAlias.nExpireTime = nTime;
	newAlias.nAcceptTransferFlags = nAcceptTransferFlags;
	bool foundRegistration = mapAliasRegistrationData.count(vchAlias) > 0;
	if (strAddress.empty() && !foundRegistration)
	{
		// generate new address in this wallet if not passed in
		CKey privKey;
		privKey.MakeNewKey(true);
		CPubKey pubKey = privKey.GetPubKey();
		vector<unsigned char> vchPubKey(pubKey.begin(), pubKey.end());
		CSyscoinAddress addressAlias(pubKey.GetID());
		strAddress = addressAlias.ToString();
		if (pwalletMain && !pwalletMain->AddKeyPubKey(privKey, pubKey))
			throw runtime_error("SYSCOIN_ALIAS_RPC_ERROR: ERRCODE: 5507 - " + _("Error adding key to wallet"));
	}
	CScript scriptPubKeyOrig;
	DecodeBase58(strAddress, newAlias.vchAddress);

	vector<unsigned char> data;
	vector<unsigned char> vchHashAlias, vchHashAlias1;
	uint256 hash;
	bool bActivation = false;
	newAlias1 = newAlias;
	if (foundRegistration)
	{
		data = mapAliasRegistrationData[vchAlias];
		hash = Hash(data.begin(), data.end());
		vchHashAlias = vchFromValue(hash.GetHex());
		if (!newAlias.UnserializeFromData(data, vchHashAlias))
			throw runtime_error("SYSCOIN_ALIAS_RPC_ERROR: ERRCODE: 5508 - " + _("Cannot unserialize alias registration transaction"));
		if (strAddress.empty())
			newAlias1.vchAddress = newAlias.vchAddress;

		newAlias1.vchGUID = newAlias.vchGUID;
		newAlias1.nExpireTime = newAlias.nExpireTime;
	}
	
	// ensure that the stored alias registration and the creation of alias from parameters matches hash, if not then the params must have changed so re-register
	newAlias1.Serialize(data);
	hash = Hash(data.begin(), data.end());
	vchHashAlias1 = vchFromValue(hash.GetHex());
	// vchHashAlias might be empty anyway if this is an initial registration or if they need to re-register as per the comments above
	if (vchHashAlias1 == vchHashAlias)
		bActivation = true;
	else {
		// if this is a re-registration we should remove old one first
		mapAliasRegistrationData.erase(vchAlias);
		mapAliasRegistrations.erase(vchHashAlias);
		mapAliasRegistrationData.insert(make_pair(vchAlias, data));
	}
	
	CScript scriptPubKey;
	if (bActivation)
		scriptPubKey << CScript::EncodeOP_N(OP_SYSCOIN_ALIAS) << CScript::EncodeOP_N(OP_ALIAS_ACTIVATE) << vchAlias << newAlias1.vchGUID << vchHashAlias1 << vchWitness << OP_2DROP << OP_2DROP << OP_2DROP;
	else
		scriptPubKey << CScript::EncodeOP_N(OP_SYSCOIN_ALIAS) << CScript::EncodeOP_N(OP_ALIAS_ACTIVATE) << vchHashAlias1 << OP_2DROP << OP_DROP;

	CSyscoinAddress newAddress;
	GetAddress(newAlias1, &newAddress, scriptPubKeyOrig);
	scriptPubKey += scriptPubKeyOrig;

	vector<CRecipient> vecSend;
	CRecipient recipient;
	CreateAliasRecipient(scriptPubKey, recipient);
	
	CScript scriptData;

	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	// calculate a fee if renewal is larger than default.. based on how many years you extend for it will be exponentially more expensive
	uint64_t nTimeExpiry = nTime - chainActive.Tip()->GetMedianTimePast();
	if (nTimeExpiry < 3600)
		nTimeExpiry = 3600;
	float fYears = nTimeExpiry / ONE_YEAR_IN_SECONDS;
	if (fYears < 1)
		fYears = 1;
	fee.nAmount = GetDataFee(scriptData) * powf(2.88, fYears);
	auto it = mapAliasRegistrations.find(vchHashAlias1);
	if (bActivation && it != mapAliasRegistrations.end())
	{
		LOCK(cs_main);
		const COutPoint &regOut = it->second;
		if (pwalletMain)
			pwalletMain->UnlockCoin(regOut);
		vecSend.push_back(fee);
		// add the registration input to the alias activation transaction
		CCoinsViewCache view(pcoinsTip);
		
		const Coin &pcoin = view.AccessCoin(regOut);
		if (pcoin.IsSpent()) {
			throw runtime_error("SYSCOIN_ALIAS_RPC_ERROR: ERRCODE: 5508 - " + _("Cannot find alias registration transaction, please ensure it has confirmed or re-submit the registration transaction again"));
		}
		tx.vin.push_back(CTxIn(regOut, pcoin.out.scriptPubKey));
		for (unsigned int i = 0; i<MAX_ALIAS_UPDATES_PER_BLOCK; i++)
		{
			vecSend.push_back(recipient);
		}
		if (!vchWitness.empty())
		{
			COutPoint aliasOutPointWitness;
			aliasunspent(vchWitness, aliasOutPointWitness);
			if (aliasOutPointWitness.IsNull())
			{
				throw runtime_error("SYSCOIN_RPC_ERROR ERRCODE: 5509 - " + _("This transaction requires a witness but not enough outputs found for witness alias: ") + stringFromVch(vchWitness));
			}
			const Coin &pcoinW = view.AccessCoin(aliasOutPointWitness);
			if (pcoinW.IsSpent()) {
				throw runtime_error("SYSCOIN_ALIAS_RPC_ERROR: ERRCODE: 5510 - " + _("Cannot find witness transaction"));
			}
			tx.vin.push_back(CTxIn(aliasOutPointWitness, pcoinW.out.scriptPubKey));
		}
	}
	else
		vecSend.push_back(recipient);
	for (auto& recp : vecSend) {
		tx.vout.push_back(CTxOut(recp.nAmount, recp.scriptPubKey));
	}
	UniValue res(UniValue::VARR);
	res.push_back(EncodeHexTx(tx));
	return res;
}
UniValue aliasupdate(const JSONRPCRequest& request) {
	const UniValue &params = request.params;
	if (request.fHelp || 8 != params.size())
		throw runtime_error(
			"aliasupdate [aliasname] [public value] [address] [accept_transfers_flags=3] [expire_timestamp] [encryption_privatekey] [encryption_publickey] [witness]\n"
						"Update and possibly transfer an alias.\n"
						"<aliasname> alias name.\n"
						"<public_value> alias public profile data, 256 characters max.\n"			
						"<address> Address of alias.\n"		
						"<accept_transfers_flags> 0 for none, 1 for accepting certificate transfers, 2 for accepting asset transfers and 3 for all. Default is 3.\n"
						"<expire_timestamp> Epoch time when to expire alias. It is exponentially more expensive per year, calculation is 2.88^years. FEERATE is the dynamic satoshi per byte fee set in the rate peg alias used for this alias. Defaults to 1 hour. Set to 0 if not changing expiration.\n"		
						"<encryption_privatekey> Encrypted private key used for encryption/decryption of private data related to this alias. If transferring, the key should be encrypted to alias_pubkey.\n"
						"<encryption_publickey> Public key used for encryption/decryption of private data related to this alias. Useful if you are changing pub/priv keypair for encryption on this alias.\n"						
						"<witness> Witness alias name that will sign for web-of-trust notarization of this transaction.\n"	
						+ HelpRequiringPassphrase());
	vector<unsigned char> vchAlias = vchFromString(params[0].get_str());
	string strPrivateValue = "";
	string strPublicValue = "";
	strPublicValue = params[1].get_str();
	
	CWalletTx wtx;
	CAliasIndex updateAlias;
	string strAddress = "";
	strAddress = params[2].get_str();
	
	unsigned char nAcceptTransferFlags = params[3].get_int();
	
	uint64_t nTime = chainActive.Tip()->GetMedianTimePast() +ONE_YEAR_IN_SECONDS;
	nTime = params[4].get_int64();

	string strEncryptionPrivateKey = "";
	strEncryptionPrivateKey = params[5].get_str();
	
	string strEncryptionPublicKey = "";
	strEncryptionPublicKey = params[6].get_str();
	
	vector<unsigned char> vchWitness;
	vchWitness = vchFromValue(params[7]);


	CAliasIndex theAlias;
	if (!GetAlias(vchAlias, theAlias))
		throw runtime_error("SYSCOIN_ALIAS_RPC_ERROR: ERRCODE: 5511 - " + _("Could not find an alias with this name"));


	CAliasIndex copyAlias = theAlias;
	theAlias.ClearAlias();
	if(strPublicValue != stringFromVch(copyAlias.vchPublicValue))
		theAlias.vchPublicValue = vchFromString(strPublicValue);
	if(strEncryptionPrivateKey != HexStr(copyAlias.vchEncryptionPrivateKey))
		theAlias.vchEncryptionPrivateKey = ParseHex(strEncryptionPrivateKey);
	if(strEncryptionPublicKey != HexStr(copyAlias.vchEncryptionPublicKey))
		theAlias.vchEncryptionPublicKey = ParseHex(strEncryptionPublicKey);

	if(strAddress != EncodeBase58(copyAlias.vchAddress))
		DecodeBase58(strAddress, theAlias.vchAddress);
	theAlias.nExpireTime = nTime;
	theAlias.nAccessFlags = copyAlias.nAccessFlags;
	theAlias.nAcceptTransferFlags = nAcceptTransferFlags;
	
	CSyscoinAddress newAddress;
	CScript scriptPubKeyOrig;
	if(theAlias.vchAddress.empty())
		GetAddress(copyAlias, &newAddress, scriptPubKeyOrig);
	else
		GetAddress(theAlias, &newAddress, scriptPubKeyOrig);

	vector<unsigned char> data;
	theAlias.Serialize(data);
    uint256 hash = Hash(data.begin(), data.end());
    vector<unsigned char> vchHashAlias = vchFromValue(hash.GetHex());

	CScript scriptPubKey;
	scriptPubKey << CScript::EncodeOP_N(OP_SYSCOIN_ALIAS) << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << copyAlias.vchAlias << copyAlias.vchGUID << vchHashAlias << vchWitness << OP_2DROP << OP_2DROP << OP_2DROP;
	scriptPubKey += scriptPubKeyOrig;

    vector<CRecipient> vecSend;
	CRecipient recipient;
	CreateAliasRecipient(scriptPubKey, recipient);
	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	if (nTime > 0) {
		// calculate a fee if renewal is larger than default.. based on how many years you extend for it will be exponentially more expensive
		uint64_t nTimeExpiry = nTime - chainActive.Tip()->GetMedianTimePast();
		if (nTimeExpiry < 3600)
			nTimeExpiry = 3600;
		float fYears = nTimeExpiry / ONE_YEAR_IN_SECONDS;
		if (fYears < 1)
			fYears = 1;
		fee.nAmount = GetDataFee(scriptData) * powf(2.88, fYears);
	}
	
	vecSend.push_back(fee);
	vecSend.push_back(recipient);
	
	bool transferAlias = false;
	if(newAddress.ToString() != EncodeBase58(copyAlias.vchAddress))
		transferAlias = true;
	return syscointxfund_helper(vchAlias, vchWitness, recipient, vecSend, transferAlias);
}
UniValue syscoindecoderawtransaction(const JSONRPCRequest& request) {
	const UniValue &params = request.params;
	if (request.fHelp || 1 != params.size())
		throw runtime_error("syscoindecoderawtransaction <hexstring>\n"
		"Decode raw syscoin transaction (serialized, hex-encoded) and display information pertaining to the service that is included in the transactiion data output(OP_RETURN)\n"
				"<hexstring> The transaction hex string.\n");
	string hexstring = params[0].get_str();
	CMutableTransaction tx;
	DecodeHexTx(tx,hexstring);
	CTransaction rawTx(tx);
	if(rawTx.IsNull())
	{
		throw runtime_error("SYSCOIN_RPC_ERROR: ERRCODE: 5512 - " + _("Could not decode transaction"));
	}
	vector<unsigned char> vchData;
	int nOut;
	int op;
	vector<vector<unsigned char> > vvch;
	vector<unsigned char> vchHash;
	GetSyscoinData(rawTx, vchData, vchHash, nOut);	
	UniValue output(UniValue::VOBJ);
	char type;
	if(DecodeAndParseSyscoinTx(rawTx, op,  vvch, type))
		SysTxToJSON(op, vchData, vchHash, output, type);
	
	return output;
}
void SysTxToJSON(const int op, const vector<unsigned char> &vchData, const vector<unsigned char> &vchHash, UniValue &entry, const char& type)
{
	if(type == ALIAS)
		AliasTxToJSON(op, vchData, vchHash, entry);
	else if(type == CERT)
		CertTxToJSON(op, vchData, vchHash, entry);
	else if(type == ESCROW)
		EscrowTxToJSON(op, vchData, vchHash, entry);
	else if(type == OFFER)
		OfferTxToJSON(op, vchData, vchHash, entry);
	else if (type == ASSET)
		AssetTxToJSON(op, vchData, vchHash, entry);
	else if (type == ASSETALLOCATION)
		AssetAllocationTxToJSON(op, vchData, vchHash, entry);
}
void AliasTxToJSON(const int op, const vector<unsigned char> &vchData, const vector<unsigned char> &vchHash, UniValue &entry)
{
	string opName = aliasFromOp(op);
	CAliasIndex alias;
	if(!alias.UnserializeFromData(vchData, vchHash))
		return;
	CAliasIndex dbAlias;
	GetAlias(alias.vchAlias, dbAlias);

	entry.push_back(Pair("txtype", opName));
	entry.push_back(Pair("_id", stringFromVch(alias.vchAlias)));
	if (!alias.offerWhitelist.IsNull())
	{
		if (alias.offerWhitelist.entries.begin()->second.nDiscountPct == 127)
			entry.push_back(Pair("whitelist", _("Whitelist was cleared")));
		else
			entry.push_back(Pair("whitelist", _("Whitelist entries were added or removed")));
	}
	if(!alias.vchPublicValue .empty() && alias.vchPublicValue != dbAlias.vchPublicValue)
		entry.push_back(Pair("publicvalue", stringFromVch(alias.vchPublicValue)));

	if(EncodeBase58(alias.vchAddress) != EncodeBase58(dbAlias.vchAddress))
		entry.push_back(Pair("address", EncodeBase58(alias.vchAddress))); 

	if(alias.nExpireTime != dbAlias.nExpireTime)
		entry.push_back(Pair("renewal", alias.nExpireTime));

}
UniValue syscoinsendrawtransaction(const JSONRPCRequest& request) {
	const UniValue &params = request.params;
	if (request.fHelp || params.size() < 1 || params.size() > 3)
		throw runtime_error("syscoinsendrawtransaction \"hexstring\" ( allowhighfees instantsend )\n"
			"\nSubmits raw transaction (serialized, hex-encoded) to local node and network.\n"
			"\nAlso see createrawtransaction and signrawtransaction calls.\n"
			"\nArguments:\n"
			"1. \"hexstring\"    (string, required) The hex string of the raw transaction)\n"
			"2. allowhighfees  (boolean, optional, default=false) Allow high fees\n"
			"3. instantsend    (boolean, optional, default=false) Use InstantSend to send this transaction\n");
	RPCTypeCheck(params, boost::assign::list_of(UniValue::VSTR)(UniValue::VBOOL)(UniValue::VBOOL));
	const string &hexstring = params[0].get_str();
	bool fOverrideFees = false;
	if (params.size() > 1)
		fOverrideFees = params[1].get_bool();

	bool fInstantSend = false;
	if (params.size() > 2)
		fInstantSend = params[2].get_bool();
	CMutableTransaction txIn;
	if (!DecodeHexTx(txIn, hexstring))
		throw runtime_error("SYSCOIN_ALIAS_RPC_ERROR: ERRCODE: 5513 - " + _("Could not send raw transaction: Cannot decode transaction from hex string"));
	CTransaction tx(txIn);
	if (tx.vin.size() <= 0)
		throw runtime_error("SYSCOIN_ALIAS_RPC_ERROR: ERRCODE: 5514 - " + _("Could not send raw transaction: Inputs are empty"));
	if (tx.vout.size() <= 0)
		throw runtime_error("SYSCOIN_ALIAS_RPC_ERROR: ERRCODE: 5515 - " + _("Could not send raw transaction: Outputs are empty"));
	UniValue arraySendParams(UniValue::VARR);
	arraySendParams.push_back(hexstring);
	arraySendParams.push_back(fOverrideFees);
	arraySendParams.push_back(fInstantSend);
	UniValue returnRes;
	try
	{
		JSONRPCRequest request;
		request.params = arraySendParams;
		returnRes = sendrawtransaction(request);
	}
	catch (UniValue& objError)
	{
		throw runtime_error(find_value(objError, "message").get_str());
	}
	if (!returnRes.isStr())
		throw runtime_error("SYSCOIN_ALIAS_RPC_ERROR: ERRCODE: 5516 - " + _("Could not send raw transaction: Invalid response from sendrawtransaction"));
	UniValue res(UniValue::VOBJ);
	res.push_back(Pair("txid", returnRes.get_str()));
	// check for alias registration, if so save the info in this node for alias activation calls after a block confirmation
	vector<vector<unsigned char> > vvch;
	int op;
	for (unsigned int i = 0; i < tx.vout.size(); i++) {
		const CTxOut& out = tx.vout[i];
		if (DecodeAliasScript(out.scriptPubKey, op, vvch) && op == OP_ALIAS_ACTIVATE) 
		{
			if(vvch.size() == 1)
			{
				auto it = mapAliasRegistrations.find(vvch[0]);
				if (it == mapAliasRegistrations.end()) {
					COutPoint prevOut(tx.GetHash(), i);
					mapAliasRegistrations.insert(make_pair(vvch[0], prevOut));
					if (pwalletMain)
						pwalletMain->LockCoin(prevOut);
				}
				

			}
			else if(vvch.size() >= 3)
			{
				mapAliasRegistrations.erase(vvch[2]);
				mapAliasRegistrationData.erase(vvch[0]);
			}
			break;
		}
	}
	
	return res;
}
string GenerateSyscoinGuid()
{
	int64_t rand = GetRand(std::numeric_limits<int64_t>::max());
	vector<unsigned char> vchGuidRand = CScriptNum(rand).getvch();
	return HexStr(vchGuidRand);
}
UniValue prunesyscoinservices(const JSONRPCRequest& request)
{
	int servicesCleaned = 0;
	CleanupSyscoinServiceDatabases(servicesCleaned);
	UniValue res(UniValue::VOBJ);
	res.push_back(Pair("services_cleaned", servicesCleaned));
	return res;
}
UniValue aliasbalance(const JSONRPCRequest& request)
{
	const UniValue &params = request.params;
    if (request.fHelp || params.size() != 1)
        throw runtime_error(
            "aliasbalance \"alias\"\n"
            "\nReturns the total amount received by the given alias in transactions.\n"
            "\nArguments:\n"
            "1. \"alias\"  (string, required) The syscoin alias for transactions.\n"
       );
	vector<unsigned char> vchAlias = vchFromValue(params[0]);
	CAmount nAmount = 0;
	CAliasIndex theAlias;
	if (!GetAlias(vchAlias, theAlias))
	{
		UniValue res(UniValue::VOBJ);
		res.push_back(Pair("balance", ValueFromAmount(nAmount)));
		return  res;
	}

	const string &strAddressFrom = EncodeBase58(theAlias.vchAddress);
	UniValue paramsUTXO(UniValue::VARR);
	UniValue param(UniValue::VOBJ);
	UniValue utxoParams(UniValue::VARR);
	utxoParams.push_back(strAddressFrom);
	param.push_back(Pair("addresses", utxoParams));
	paramsUTXO.push_back(param);
	JSONRPCRequest request1;
	request1.params = paramsUTXO;
	const UniValue &resUTXOs = getaddressutxos(request1);
	UniValue utxoArray(UniValue::VARR);
	if (resUTXOs.isArray())
		utxoArray = resUTXOs.get_array();
	else
	{
		UniValue res(UniValue::VOBJ);
		res.push_back(Pair("balance", ValueFromAmount(nAmount)));
		return  res;
	}
	{
		LOCK(mempool.cs);
		int op;
		vector<vector<unsigned char> > vvch;
		for (unsigned int i = 0; i < utxoArray.size(); i++)
		{
			const UniValue& utxoObj = utxoArray[i].get_obj();
			const uint256& txid = uint256S(find_value(utxoObj, "txid").get_str());
			const int& nOut = find_value(utxoObj, "outputIndex").get_int();
			const std::vector<unsigned char> &data(ParseHex(find_value(utxoObj, "script").get_str()));
			const CScript& scriptPubKey = CScript(data.begin(), data.end());
			const CAmount &nValue = find_value(utxoObj, "satoshis").get_int64();
			const int& nHeight = find_value(utxoObj, "height").get_int();
			const COutPoint outPoint(txid, nOut);
			if (DecodeAliasScript(scriptPubKey, op, vvch))
				continue;
			if (mempool.mapNextTx.find(outPoint) != mempool.mapNextTx.end())
				continue;
			if (!IsOutpointMature(outPoint))
				continue;
			nAmount += nValue;

		}
	}
	UniValue res(UniValue::VOBJ);
	res.push_back(Pair("balance", ValueFromAmount(nAmount)));
    return  res;
}
/**
 * [aliasinfo description]
 * @param  params [description]
 * @param  fHelp  [description]
 * @return        [description]
 */
UniValue aliasinfo(const JSONRPCRequest& request) {
	const UniValue &params = request.params;
	if (request.fHelp || 1 > params.size())
		throw runtime_error("aliasinfo <aliasname>\n"
				"Show values of an alias.\n");
	vector<unsigned char> vchAlias = vchFromValue(params[0]);
	CAliasIndex txPos;
	if (!paliasdb || !paliasdb->ReadAlias(vchAlias, txPos))
		throw runtime_error("SYSCOIN_ALIAS_RPC_ERROR: ERRCODE: 5517 - " + _("Failed to read from alias DB"));

	UniValue oName(UniValue::VOBJ);
	if(!BuildAliasJson(txPos, oName))
		throw runtime_error("SYSCOIN_ALIAS_RPC_ERROR: ERRCODE: 5518 - " + _("Could not find this alias"));
		
	return oName;
}
bool BuildAliasJson(const CAliasIndex& alias, UniValue& oName)
{
	bool expired = false;
	int64_t expired_time = 0;
	oName.push_back(Pair("_id", stringFromVch(alias.vchAlias)));
	oName.push_back(Pair("encryption_privatekey", HexStr(alias.vchEncryptionPrivateKey)));
	oName.push_back(Pair("encryption_publickey", HexStr(alias.vchEncryptionPublicKey)));
	oName.push_back(Pair("publicvalue", stringFromVch(alias.vchPublicValue)));	
	oName.push_back(Pair("txid", alias.txHash.GetHex()));
	int64_t nTime = 0;
	if (chainActive.Height() >= alias.nHeight) {
		CBlockIndex *pindex = chainActive[alias.nHeight];
		if (pindex) {
			nTime = pindex->GetMedianTimePast();
		}
	}
	oName.push_back(Pair("time", nTime));
	oName.push_back(Pair("address", EncodeBase58(alias.vchAddress)));
	oName.push_back(Pair("accepttransferflags", (int)alias.nAcceptTransferFlags));
	expired_time = alias.nExpireTime;
	if(expired_time <= chainActive.Tip()->GetMedianTimePast())
	{
		expired = true;
	}  
	oName.push_back(Pair("expires_on", expired_time));
	oName.push_back(Pair("expired", expired));
	return true;
}
bool BuildAliasIndexerHistoryJson(const CAliasIndex& alias, UniValue& oName)
{
	oName.push_back(Pair("_id", alias.txHash.GetHex()));
	oName.push_back(Pair("publicvalue", stringFromVch(alias.vchPublicValue)));
	oName.push_back(Pair("alias", stringFromVch(alias.vchAlias)));
	int64_t nTime = 0; 
	if (chainActive.Height() >= alias.nHeight) {
		CBlockIndex *pindex = chainActive[alias.nHeight];
		if (pindex) {
			nTime = pindex->GetMedianTimePast();
		}
	}
	oName.push_back(Pair("time", nTime));
	oName.push_back(Pair("address", EncodeBase58(alias.vchAddress)));
	oName.push_back(Pair("accepttransferflags", (int)alias.nAcceptTransferFlags));
	return true;
}
unsigned int aliasunspent(const vector<unsigned char> &vchAlias, COutPoint& outpoint)
{
	outpoint.SetNull();
	CAliasIndex theAlias;
	if (!GetAlias(vchAlias, theAlias))
		return 0;
	UniValue paramsUTXO(UniValue::VARR);
	UniValue param(UniValue::VOBJ);
	UniValue utxoParams(UniValue::VARR);
	const string &strAddressFrom = EncodeBase58(theAlias.vchAddress);
	utxoParams.push_back(strAddressFrom);
	param.push_back(Pair("addresses", utxoParams));
	paramsUTXO.push_back(param);
	JSONRPCRequest request;
	request.params = paramsUTXO;
	const UniValue &resUTXOs = getaddressutxos(request);
	UniValue utxoArray(UniValue::VARR);
	if (resUTXOs.isArray())
		utxoArray = resUTXOs.get_array();
	else
		return 0;
	unsigned int count = 0;
	CAmount nCurrentAmount = 0;
	{
		LOCK(mempool.cs);
		for (unsigned int i = 0; i < utxoArray.size(); i++)
		{
			const UniValue& utxoObj = utxoArray[i].get_obj();
			const uint256& txid = uint256S(find_value(utxoObj, "txid").get_str());
			const int& nOut = find_value(utxoObj, "outputIndex").get_int();
			const std::vector<unsigned char> &data(ParseHex(find_value(utxoObj, "script").get_str()));
			const CScript& scriptPubKey = CScript(data.begin(), data.end());
			int op;
			vector<vector<unsigned char> > vvch;
			if (!DecodeAliasScript(scriptPubKey, op, vvch) || vvch.size() <= 1 || vvch[0] != theAlias.vchAlias || vvch[1] != theAlias.vchGUID)
				continue;
			const COutPoint &outPointToCheck = COutPoint(txid, nOut);

			if (mempool.mapNextTx.find(outPointToCheck) != mempool.mapNextTx.end())
				continue;

			if (outpoint.IsNull())
				outpoint = outPointToCheck;
			count++;
		}
	}
	return count;
}
void aliasselectpaymentcoins(const vector<unsigned char> &vchAlias, const CAmount &nAmount, vector<COutPoint>& outPoints, CAmount &nRequiredAmount)
{
	nRequiredAmount = 0;
	int numResults = 0;
	CAmount nCurrentAmount = 0;
	CAmount nDesiredAmount = nAmount;
	outPoints.clear();
	CAliasIndex theAlias;
	if (!GetAlias(vchAlias, theAlias))
		return;

	const string &strAddressFrom = EncodeBase58(theAlias.vchAddress);
	UniValue paramsUTXO(UniValue::VARR);
	UniValue param(UniValue::VOBJ);
	UniValue utxoParams(UniValue::VARR);
	utxoParams.push_back(strAddressFrom);
	param.push_back(Pair("addresses", utxoParams));
	paramsUTXO.push_back(param);
	JSONRPCRequest request;
	request.params = paramsUTXO;
	const UniValue &resUTXOs = getaddressutxos(request);
	UniValue utxoArray(UniValue::VARR);
	if (resUTXOs.isArray())
		utxoArray = resUTXOs.get_array();
	else
		return;

	int op;
	vector<vector<unsigned char> > vvch;
	bool bIsFunded = false;
	CAmount nFeeRequired = 0;
	const CAmount &outputFee = CWallet::GetMinimumFee(200u, nTxConfirmTarget, mempool);
	for (unsigned int i = 0; i<utxoArray.size(); i++)
	{
		const UniValue& utxoObj = utxoArray[i].get_obj();
		const uint256& txid = uint256S(find_value(utxoObj, "txid").get_str());
		const int& nOut = find_value(utxoObj, "outputIndex").get_int();
		const std::vector<unsigned char> &data(ParseHex(find_value(utxoObj, "script").get_str()));
		const CScript& scriptPubKey = CScript(data.begin(), data.end());
		const CAmount &nValue = find_value(utxoObj, "satoshis").get_int64();
		const COutPoint &outPointToCheck = COutPoint(txid, nOut);
		if (DecodeAliasScript(scriptPubKey, op, vvch) && vvch.size() > 1 && vvch[0] == theAlias.vchAlias && vvch[1] == theAlias.vchGUID) 
			continue;
		if (pwalletMain && pwalletMain->IsLockedCoin(txid, nOut))
			continue;
		{
			LOCK(mempool.cs);
			if (mempool.mapNextTx.find(outPointToCheck) != mempool.mapNextTx.end())
				continue;
		}
		if (!IsOutpointMature(outPointToCheck))
			continue;
		// add min fee for every input
		nFeeRequired += outputFee;
		outPoints.push_back(outPointToCheck);
		nCurrentAmount += nValue;
		if (nCurrentAmount >= (nDesiredAmount + nFeeRequired)) {
			bIsFunded = true;
			break;
		}
		else
			bIsFunded = false;

	}
	if (!bIsFunded) {
		nRequiredAmount = (nDesiredAmount + nFeeRequired) - nCurrentAmount;
		if (nRequiredAmount < 0)
			nRequiredAmount = 0;
	}
}
UniValue aliaspay_helper(const vector<unsigned char> &vchAlias, vector<CRecipient> &vecSend, bool instantsend) {
	CWalletTx wtxNew1, wtxNew2;
	CReserveKey reservekey(pwalletMain);
	CAmount nFeeRequired;
	std::string strError;
	int nChangePosRet = -1;
	CCoinControl coinControl;
	coinControl.fAllowOtherInputs = false;
	coinControl.fAllowWatchOnly = false;
	// get total output required
	// if aliasRecipient.scriptPubKey.empty() then it is not a syscoin tx, so don't set tx flag for syscoin tx in createtransaction()
	// this is because aliasRecipient means an alias utxo was used to create a transaction, common to every syscoin service transaction, aliaspay doesn't use an alias utxo it just sends money from address to address but uses alias outputs to fund it and sign externally using zero knowledge auth.
	if (!pwalletMain->CreateTransaction(vecSend, wtxNew1, reservekey, nFeeRequired, nChangePosRet, strError, &coinControl, false, ALL_COINS, instantsend)) {
		throw runtime_error(strError);
	}


	CAmount nOutputTotal = 0;
	BOOST_FOREACH(const CRecipient& recp, vecSend)
	{
		nOutputTotal += recp.nAmount;
	}
	const CAmount &outputFee = CWallet::GetMinimumFee(200u, nTxConfirmTarget, mempool);
	// account for 1 change output
	nFeeRequired += outputFee;
	CAmount nTotal = nOutputTotal + nFeeRequired;
	if (nTotal > 0)
	{
		vector<COutPoint> outPoints;
		// select just get enough outputs to fund nTotal
		aliasselectpaymentcoins(vchAlias, nTotal, outPoints, nFeeRequired);
		if (nFeeRequired > 0)
			throw runtime_error("SYSCOIN_RPC_ERROR ERRCODE: 9000 - " + _("The Syscoin Alias does not have enough funds to complete this transaction. You need to deposit the following amount of coins in order for the transaction to succeed: ") + ValueFromAmount(nFeeRequired).write());
		BOOST_FOREACH(const COutPoint& outpoint, outPoints)
		{
			if (!coinControl.IsSelected(outpoint))
				coinControl.Select(outpoint);
		}
	}

	// now create the transaction and fake sign with enough funding from alias utxo's (if coinControl specified fAllowOtherInputs(true) then and only then are wallet inputs are allowed)
	// actual signing happens in signrawtransaction outside of this function call after the wtxNew raw transaction is returned back to it
	if (!pwalletMain->CreateTransaction(vecSend, wtxNew2, reservekey, nFeeRequired, nChangePosRet, strError, &coinControl, false, ALL_COINS, instantsend)) {
		throw runtime_error(strError);
	}
	UniValue res(UniValue::VARR);
	res.push_back(EncodeHexTx(*wtxNew2.tx));
	return res;
}
UniValue aliaspay(const JSONRPCRequest& request) {
	const UniValue &params = request.params;
    if (request.fHelp || params.size() < 2 || params.size() > 4)
        throw runtime_error(
            "aliaspay aliasfrom {\"address\":amount,...} (instantsend subtractfeefromamount)\n"
            "\nSend multiple times from an alias. Amounts are double-precision floating point numbers."
            + HelpRequiringPassphrase() + "\n"
            "\nArguments:\n"
			"1. \"aliasfrom\"			(string, required) Alias to pay from\n"
            "2. \"amounts\"             (string, required) A json object with aliases and amounts\n"
            "    {\n"
            "      \"address\":amount   (numeric or string) The syscoin address is the key, the numeric amount (can be string) in SYS is the value\n"
            "      ,...\n"
            "    }\n"
			"3. instantsend				(boolean, optional) Set to true to use InstantSend to send this transaction or false otherwise. Default is false.\n"
			"4. subtractfeefromamount   (string, optional) A json array with addresses.\n"
			"                           The fee will be equally deducted from the amount of each selected address.\n"
			"                           Those recipients will receive less syscoins than you enter in their corresponding amount field.\n"
			"                           If no addresses are specified here, the sender pays the fee.\n"
			"    [\n"
			"      \"address\"            (string) Subtract fee from this address\n"
			"      ,...\n"
			"    ]\n"
            "\nResult:\n"
			"\"transaction hex\"          (string) The transaction hex (unsigned) for signing and sending. Only 1 transaction is created regardless of \n"
            "                                    the number of addresses.\n"
			"\nExamples:\n"
			"\nSend two amounts to two different aliases:\n"
			+ HelpExampleCli("aliaspay", "\"senderalias\" \"{\\\"alias1\\\":0.01,\\\"alias2\\\":0.02}\"") +
			"\nSend two amounts to two different addresses setting IS and comment:\n"
			+ HelpExampleCli("aliaspay", "\"senderalias\" \"{\\\"Sa8H1Mq4pd6z3N4xFzxvVah9AWzZyykJiJ\\\":0.01,\\\"SkbcpmjqkERwvPPqke3puu9k9bCdHLaVoP\\\":0.02}\" true \"testing\"") +
			"\nSend two amounts to two different addresses, subtract fee from amount:\n"
			+ HelpExampleCli("aliaspay", "\"senderalias\" \"{\\\"Sa8H1Mq4pd6z3N4xFzxvVah9AWzZyykJiJ\\\":0.01,\\\"SkbcpmjqkERwvPPqke3puu9k9bCdHLaVoP\\\":0.02}\" false \"\" \"[\\\"Sa8H1Mq4pd6z3N4xFzxvVah9AWzZyykJiJ\\\",\\\"SkbcpmjqkERwvPPqke3puu9k9bCdHLaVoP\\\"]\"") +
			"\nAs a json rpc call\n"
			+ HelpExampleRpc("aliaspay", "\"senderalias\", {\"Sa8H1Mq4pd6z3N4xFzxvVah9AWzZyykJiJ\":0.01,\"SkbcpmjqkERwvPPqke3puu9k9bCdHLaVoP\":0.02}, false, \"testing\"")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    string strFrom = params[0].get_str();
	CAliasIndex theAlias;
	if (!GetAlias(vchFromString(strFrom), theAlias))
		throw runtime_error("SYSCOIN_ALIAS_RPC_ERROR: ERRCODE: 5519 - " + _("Invalid fromalias"));

    UniValue sendTo = params[1].get_obj();

	bool fUseInstantSend = false;
	if (params.size() > 2)
		fUseInstantSend = params[2].get_bool();

	UniValue subtractFeeFromAmount(UniValue::VARR);
	if (params.size() > 3)
		subtractFeeFromAmount = params[3].get_array();


	CWalletTx wtx;
    set<CSyscoinAddress> setAddress;
    vector<CRecipient> vecSend;

    CAmount totalAmount = 0;
    vector<string> keys = sendTo.getKeys();
    BOOST_FOREACH(const string& name_, keys)
    {
        CSyscoinAddress address(name_);
		vector<unsigned char> vchPubKey;
		string strAddress = name_;
		if (GetAddressFromAlias(name_, strAddress, vchPubKey))
		{
			address = CSyscoinAddress(strAddress);
		}
        if (!address.IsValid())
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, string("Invalid Syscoin address: ")+name_);

        if (setAddress.count(address))
            throw JSONRPCError(RPC_INVALID_PARAMETER, string("Invalid parameter, duplicated address: ")+name_);
        setAddress.insert(address);
        CScript scriptPubKey = GetScriptForDestination(address.Get());
		CAmount nAmount = AmountFromValue(sendTo[name_]);
		if (nAmount <= 0)
			throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount for send");
        totalAmount += nAmount;
		bool fSubtractFeeFromAmount = false;
		for (unsigned int idx = 0; idx < subtractFeeFromAmount.size(); idx++) {
			const UniValue& addr = subtractFeeFromAmount[idx];
			if (addr.get_str() == strAddress)
				fSubtractFeeFromAmount = true;
		}
        CRecipient recipient = {scriptPubKey, nAmount, fSubtractFeeFromAmount };
        vecSend.push_back(recipient);
    }

    EnsureWalletIsUnlocked();
    // Check funds
	UniValue balanceParams(UniValue::VARR);
	balanceParams.push_back(strFrom);
	JSONRPCRequest request1;
	request1.params = balanceParams;
	const UniValue &resBalance = aliasbalance(request1);
	CAmount nBalance = AmountFromValue(find_value(resBalance.get_obj(), "balance"));
    if (totalAmount > nBalance)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Alias has insufficient funds");

	return aliaspay_helper(theAlias.vchAlias, vecSend, fUseInstantSend);
}
UniValue aliasaddscript(const JSONRPCRequest& request) {
	const UniValue &params = request.params;
	if (request.fHelp || 1 != params.size())
		throw runtime_error("aliasaddscript redeemscript\n"
				"Add redeemscript to local wallet for signing smart contract based alias transactions.\n");
	std::vector<unsigned char> data(ParseHex(params[0].get_str()));
	if(pwalletMain)
		pwalletMain->AddCScript(CScript(data.begin(), data.end()));
	UniValue res(UniValue::VOBJ);
	res.push_back(Pair("result", "success"));
	return res;
}
UniValue aliasupdatewhitelist(const JSONRPCRequest& request) {
	const UniValue &params = request.params;
	if (request.fHelp || params.size() != 3)
		throw runtime_error(
			"aliasupdatewhitelist [owner alias] [{\"alias\":\"aliasname\",\"discount_percentage\":n},...] [witness]\n"
			"Update to the whitelist(controls who can resell). Array of whitelist entries in parameter 1.\n"
			"To add to list, include a new alias/discount percentage that does not exist in the whitelist.\n"
			"To update entry, change the discount percentage of an existing whitelist entry.\n"
			"To remove whitelist entry, pass the whilelist entry without changing discount percentage.\n"
			"<owner alias> owner alias controlling this whitelist.\n"
			"	\"entries\"       (string) A json array of whitelist entries to add/remove/update\n"
			"    [\n"
			"      \"alias\"     (string) Alias that you want to add to the affiliate whitelist. Can be '*' to represent that the offers owned by owner alias can be resold by anybody.\n"
			"	   \"discount_percentage\"     (number) A discount percentage associated with this alias. The reseller can sell your offer at this discount, not accounting for any commissions he/she may set in his own reselling offer. 0 to 99.\n"
			"      ,...\n"
			"    ]\n"
			"<witness> Witness alias name that will sign for web-of-trust notarization of this transaction.\n"
			+ HelpRequiringPassphrase());

	// gather & validate inputs
	vector<unsigned char> vchOwnerAlias = vchFromValue(params[0]);
	UniValue whitelistEntries = params[1].get_array();
	vector<unsigned char> vchWitness;
	vchWitness = vchFromValue(params[2]);
	CWalletTx wtx;

	// this is a syscoin txn
	CScript scriptPubKeyOrig;


	CAliasIndex theAlias;
	if (!GetAlias(vchOwnerAlias, theAlias))
		throw runtime_error("SYSCOIN_ALIAS_RPC_ERROR ERRCODE: 5520 - " + _("Could not find an alias with this guid"));

	CSyscoinAddress aliasAddress;
	GetAddress(theAlias, &aliasAddress, scriptPubKeyOrig);
	CAliasIndex copyAlias = theAlias;
	theAlias.ClearAlias();

	for (unsigned int idx = 0; idx < whitelistEntries.size(); idx++) {
		const UniValue& p = whitelistEntries[idx];
		if (!p.isObject())
			throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "expected object with {\"alias\",\"discount_percentage\"}");

		UniValue whiteListEntryObj = p.get_obj();
		RPCTypeCheckObj(whiteListEntryObj, boost::assign::map_list_of("alias", UniValue::VSTR)("discount_percentage", UniValue::VNUM));
		string aliasEntryName = find_value(whiteListEntryObj, "alias").get_str();
		int nDiscount = find_value(whiteListEntryObj, "discount_percentage").get_int();

		COfferLinkWhitelistEntry entry;
		entry.aliasLinkVchRand = vchFromString(aliasEntryName);
		entry.nDiscountPct = nDiscount;
		theAlias.offerWhitelist.PutWhitelistEntry(entry);

		if (!theAlias.offerWhitelist.GetLinkEntryByHash(vchFromString(aliasEntryName), entry))
			throw runtime_error("SYSCOIN_ALIAS_RPC_ERROR ERRCODE: 5521 - " + _("This alias entry was not added to affiliate list: ") + aliasEntryName);
	}
	vector<unsigned char> data;
	theAlias.Serialize(data);
	uint256 hash = Hash(data.begin(), data.end());
	vector<unsigned char> vchHashAlias = vchFromValue(hash.GetHex());

	CScript scriptPubKey;
	scriptPubKey << CScript::EncodeOP_N(OP_SYSCOIN_ALIAS) << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << copyAlias.vchAlias << copyAlias.vchGUID << vchHashAlias << vchWitness << OP_2DROP << OP_2DROP << OP_2DROP;
	scriptPubKey += scriptPubKeyOrig;

	vector<CRecipient> vecSend;
	CRecipient recipient;
	CreateAliasRecipient(scriptPubKey, recipient);
	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);
	vecSend.push_back(recipient);

	return syscointxfund_helper(copyAlias.vchAlias, vchWitness, recipient, vecSend);
}
UniValue aliasclearwhitelist(const JSONRPCRequest& request) {
	const UniValue &params = request.params;
	if (request.fHelp || params.size() != 2)
		throw runtime_error(
			"aliasclearwhitelist [owner alias] [witness]\n"
			"Clear your whitelist(controls who can resell).\n"
			+ HelpRequiringPassphrase());
	// gather & validate inputs
	vector<unsigned char> vchAlias = vchFromValue(params[0]);
	vector<unsigned char> vchWitness;
	vchWitness = vchFromValue(params[1]);
	// this is a syscoind txn
	CWalletTx wtx;
	CScript scriptPubKeyOrig;


	CAliasIndex theAlias;
	if (!GetAlias(vchAlias, theAlias))
		throw runtime_error("SYSCOIN_ALIAS_RPC_ERROR ERRCODE: 5522 - " + _("Could not find an alias with this name"));


	CSyscoinAddress aliasAddress;
	GetAddress(theAlias, &aliasAddress, scriptPubKeyOrig);

	COfferLinkWhitelistEntry entry;
	// special case to clear all entries for this offer
	entry.nDiscountPct = 127;
	CAliasIndex copyAlias = theAlias;
	theAlias.ClearAlias();
	theAlias.offerWhitelist.PutWhitelistEntry(entry);
	vector<unsigned char> data;
	theAlias.Serialize(data);
	uint256 hash = Hash(data.begin(), data.end());
	vector<unsigned char> vchHashAlias = vchFromValue(hash.GetHex());

	CScript scriptPubKey;
	scriptPubKey << CScript::EncodeOP_N(OP_SYSCOIN_ALIAS) << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << copyAlias.vchAlias << copyAlias.vchGUID << vchHashAlias << vchWitness << OP_2DROP << OP_2DROP << OP_2DROP;
	scriptPubKey += scriptPubKeyOrig;

	vector<CRecipient> vecSend;
	CRecipient recipient;
	CreateAliasRecipient(scriptPubKey, recipient);
	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);
	vecSend.push_back(recipient);

	return syscointxfund_helper(copyAlias.vchAlias, vchWitness, recipient, vecSend);
}
bool DoesAliasExist(const string &strAddress) {
	vector<unsigned char> vchMyAlias;
	vector<unsigned char> vchAddress;
	DecodeBase58(strAddress, vchAddress);
	return paliasdb->ReadAddress(vchAddress, vchMyAlias);
}
UniValue syscoinlistreceivedbyaddress(const JSONRPCRequest& request)
{
	const UniValue &params = request.params;
	if (request.fHelp || params.size() != 0)
		throw runtime_error(
			"syscoinlistreceivedbyaddress\n"
			"\nList balances by receiving address.\n"
			"\nResult:\n"
			"[\n"
			"  {\n" 
			"    \"address\" : \"receivingaddress\",    (string) The receiving address\n"
			"    \"amount\" : x.xxx,					(numeric) The total amount in " + CURRENCY_UNIT + " received by the address\n"
			"    \"label\" : \"label\"                  (string) A comment for the address/transaction, if any\n"
			"    \"alias\" : \"alias\"                  (string) Associated alias to this address, if any\n"
			"  }\n"
			"  ,...\n"
			"]\n"

			"\nExamples:\n"
			+ HelpExampleCli("syscoinlistreceivedbyaddress", "")
		);

	return SyscoinListReceived();
}
UniValue aliaswhitelist(const JSONRPCRequest& request) {
	const UniValue &params = request.params;
	if (request.fHelp || params.size() != 1)
		throw runtime_error("aliaswhitelist <alias>\n"
			"List all affiliates for this alias.\n");
	UniValue oRes(UniValue::VARR);
	vector<unsigned char> vchAlias = vchFromValue(params[0]);

	CAliasIndex theAlias;

	if (!GetAlias(vchAlias, theAlias))
		throw runtime_error("could not find alias with this guid");

	for (auto const &it : theAlias.offerWhitelist.entries)
	{
		const COfferLinkWhitelistEntry& entry = it.second;
		UniValue oList(UniValue::VOBJ);
		oList.push_back(Pair("alias", stringFromVch(entry.aliasLinkVchRand)));
		oList.push_back(Pair("discount_percentage", entry.nDiscountPct));
		oRes.push_back(oList);
	}
	return oRes;
}
bool COfferLinkWhitelist::GetLinkEntryByHash(const std::vector<unsigned char> &ahash, COfferLinkWhitelistEntry &entry) const {
	entry.SetNull();
	const vector<unsigned char> allAliases = vchFromString("*");
	if (entries.count(ahash) > 0) {
		entry = entries.at(ahash);
		return true;
	}
	else if(entries.count(allAliases) > 0) {
		entry = entries.at(allAliases);
		return true;
	}
	return false;
}
string GetSyscoinTransactionDescription(const CTransaction& tx, const int op, string& responseEnglish, const char &type, string& responseGUID)
{
	string strResponse = "";
	if (type == ALIAS) {
		CAliasIndex alias(tx);
		if (!alias.IsNull()) {
			responseGUID = stringFromVch(alias.vchAlias);
			if (op == OP_ALIAS_ACTIVATE) {
				strResponse = _("Alias Activated");
				responseEnglish = "Alias Activated";
			}
			else if (op == OP_ALIAS_UPDATE) {
				strResponse = _("Alias Updated");
				responseEnglish = "Alias Updated";
			}
		}
	}
	else if (type == OFFER) {
		COffer offer(tx);
		if (!offer.IsNull()) {
			responseGUID = stringFromVch(offer.vchOffer);
			if (op == OP_OFFER_ACTIVATE) {
				strResponse = _("Offer Activated");
				responseEnglish = "Offer Activated";
			}
			else if (op == OP_OFFER_UPDATE) {
				strResponse = _("Offer Updated");
				responseEnglish = "Offer Updated";
			}
		}
	}
	else if (type == CERT) {
		CCert cert(tx);
		if (!cert.IsNull()) {
			responseGUID = stringFromVch(cert.vchCert);
			if (op == OP_CERT_ACTIVATE) {
				strResponse = _("Certificate Activated");
				responseEnglish = "Certificate Activated";
			}
			else if (op == OP_CERT_UPDATE) {
				strResponse = _("Certificate Updated");
				responseEnglish = "Certificate Updated";
			}
			else if (op == OP_CERT_TRANSFER) {
				strResponse = _("Certificate Transferred");
				responseEnglish = "Certificate Transferred";
			}
		}
	}
	else if (type == ASSET) {
		CAsset asset(tx);
		if (!asset.IsNull()) {
			responseGUID = stringFromVch(asset.vchAsset);
			if (op == OP_ASSET_ACTIVATE) {
				strResponse = _("Asset Activated");
				responseEnglish = "Asset Activated";
			}
			else if (op == OP_ASSET_UPDATE) {
				strResponse = _("Asset Updated");
				responseEnglish = "Asset Updated";
			}
			else if (op == OP_ASSET_TRANSFER) {
				strResponse = _("Asset Transferred");
				responseEnglish = "Asset Transferred";
			}
			else if (op == OP_ASSET_SEND) {
				strResponse = _("Asset Sent");
				responseEnglish = "Asset Sent";
			}
		}
	}
	else if (type == ASSETALLOCATION) {
		CAssetAllocation assetallocation(tx);
		if (!assetallocation.IsNull()) {
			responseGUID = stringFromVch(assetallocation.vchAsset);
			if (op == OP_ASSET_ALLOCATION_SEND) {
				strResponse = _("Asset Allocation Sent");
				responseEnglish = "Asset Allocation Sent";
			}
			else if (op == OP_ASSET_COLLECT_INTEREST) {
				strResponse = _("Asset Collect Interest");
				responseEnglish = "Asset Collect Interest";
			}
		}
	}
	else if (type == ESCROW) {
		CEscrow escrow(tx);
		if (!escrow.IsNull()) {
			responseGUID = stringFromVch(escrow.vchEscrow);
			if (op == OP_ESCROW_ACTIVATE) {
				strResponse = _("Escrow Activated");
				responseEnglish = "Escrow Activated";
			}
			else if (op == OP_ESCROW_ACKNOWLEDGE) {
				strResponse = _("Escrow Acknowledged");
				responseEnglish = "Escrow Acknowledged";
			}
			else if (op == OP_ESCROW_RELEASE) {
				strResponse = _("Escrow Released");
				responseEnglish = "Escrow Released";
			}
			else if (op == OP_ESCROW_RELEASE_COMPLETE) {
				strResponse = _("Escrow Release Complete");
				responseEnglish = "Escrow Release Complete";
			}
			else if (op == OP_ESCROW_FEEDBACK) {
				strResponse = _("Escrow Feedback");
				responseEnglish = "Escrow Feedback";
			}
			else if (op == OP_ESCROW_BID) {
				strResponse = _("Escrow Bid");
				responseEnglish = "Escrow Bid";
			}
			else if (op == OP_ESCROW_ADD_SHIPPING) {
				strResponse = _("Escrow Add Shipping");
				responseEnglish = "Escrow Add Shipping";
			}
			else if (op == OP_ESCROW_REFUND) {
				strResponse = _("Escrow Refunded");
				responseEnglish = "Escrow Refunded";
			}
			else if (op == OP_ESCROW_REFUND_COMPLETE) {
				strResponse = _("Escrow Refund Complete");
				responseEnglish = "Escrow Refund Complete";
			}
		}
	}
	else{
		return "";
	}
	return strResponse + " " + responseGUID;
}
bool IsOutpointMature(const COutPoint& outpoint)
{
	Coin coin;
	GetUTXOCoin(outpoint, coin);
	if (coin.IsSpent())
		return false;
	int numConfirmationsNeeded = 2;
	if (coin.IsCoinBase())
		numConfirmationsNeeded = COINBASE_MATURITY;

	if (coin.nHeight > -1 && chainActive.Tip())
		return (chainActive.Height() - coin.nHeight) >= numConfirmationsNeeded;
	
	// don't have chainActive or coin height is neg 1 or less
	return false;

}
