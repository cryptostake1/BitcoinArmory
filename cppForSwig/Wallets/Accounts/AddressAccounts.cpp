////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2017-2021, goatpig                                          //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include "AccountTypes.h"
#include "AddressAccounts.h"
#include "../EncryptedDB.h"
#include "../WalletFileInterface.h"
#include "../DecryptedDataContainer.h"

using namespace std;


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// AddressAccount
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
void AddressAccount::make_new(
   shared_ptr<AccountType> accType,
   shared_ptr<DecryptedDataContainer> decrData,
   unique_ptr<Cipher> cipher,
   const std::function<std::shared_ptr<AssetEntry>(void)>& getRootLbd)
{
   reset();

   //create root asset
   auto createRootAsset = [&decrData, this, &getRootLbd](
      shared_ptr<AccountType_BIP32> accBip32,
      const NodeRoot& nodeRoot,
      unique_ptr<Cipher> cipher_copy)->
      shared_ptr<AssetEntry_BIP32Root>
   {
      //get last node
      const auto& derPath = DerivationTree::toPath32(nodeRoot.path);
      uint32_t node_id = 0;
      auto nodeIt = prev(derPath.end());
      if (nodeIt != derPath.end())
         node_id = *nodeIt;

      //create ids
      auto&& account_id = WRITE_UINT32_BE(node_id);
      auto&& full_account_id = ID_ + account_id;

      shared_ptr<AssetEntry_BIP32Root> rootAsset;

      //setup bip32 root object from base58 string
      BIP32_Node node;
      node.initFromBase58(nodeRoot.b58Root);

      auto chaincode = node.moveChaincode();
      auto pubkey = node.movePublicKey();

      if (node.isPublic())
      {
         //WO wallet
         rootAsset = make_shared<AssetEntry_BIP32Root>(
            -1, full_account_id,
            pubkey, nullptr,
            chaincode,
            node.getDepth(), node.getLeafID(), 
            node.getParentFingerprint(), accBip32->getSeedFingerprint(),
            derPath);
      }
      else
      {
         //full wallet
         ReentrantLock lock(decrData.get());

         //encrypt private root
         auto&& encrypted_root =
            decrData->encryptData(cipher_copy.get(), node.getPrivateKey());

         //create assets
         auto privKeyID = full_account_id;
         privKeyID.append(WRITE_UINT32_LE(UINT32_MAX));
         auto priv_asset = make_shared<Asset_PrivateKey>(
            privKeyID, encrypted_root, move(cipher_copy));

         rootAsset = make_shared<AssetEntry_BIP32Root>(
            -1, full_account_id,
            pubkey, priv_asset,
            chaincode,
            node.getDepth(), node.getLeafID(), 
            node.getParentFingerprint(), accBip32->getSeedFingerprint(),
            derPath);
      }

      return rootAsset;
   };

   //create account
   auto createNewAccount = [this](
      shared_ptr<AssetEntry_BIP32Root> rootAsset,
      shared_ptr<DerivationScheme_BIP32> derScheme)->
      shared_ptr<AssetAccountData>
   {
      if(rootAsset == nullptr)
         throw AccountException("null root asset");

      //der scheme
      if(derScheme == nullptr)
      {
         auto chaincode = rootAsset->getChaincode();
         if (chaincode.getSize() == 0)
            throw AccountException("invalid chaincode");

         derScheme = make_shared<DerivationScheme_BIP32>(
            chaincode, rootAsset->getDepth(), rootAsset->getLeafID());
      }

      //account id
      auto full_account_id = rootAsset->getAccountID();
      auto len = full_account_id.getSize();
      if (ID_.getSize() > len)
         throw AccountException("unexpected ID size");

      auto account_id = full_account_id.getSliceCopy(
         ID_.getSize(), len - ID_.getSize());

      //instantiate account
      return make_shared<AssetAccountData>(
         AssetAccountTypeEnum_Plain, account_id, ID_,
         rootAsset, derScheme, dbName_);
   };

   //body
   switch (accType->type())
   {
   case AccountTypeEnum_ArmoryLegacy:
   {
      auto accPtr = dynamic_pointer_cast<AccountType_ArmoryLegacy>(accType);
      ID_ = accPtr->getAccountID();
      auto asset_account_id = accPtr->getOuterAccountID();

      //first derived asset
      auto&& full_account_id = ID_ + asset_account_id;
      shared_ptr<AssetEntry_Single> firstAsset;

      if (!getRootLbd)
         throw AccountException("[make_new] undefined root lbd");
      auto rootPtr = getRootLbd();
      auto root135 = dynamic_pointer_cast<AssetEntry_ArmoryLegacyRoot>(rootPtr);
      if (root135 == nullptr)
         throw AccountException("[make_new] expected legacy root");

      //chaincode has to be a copy cause the derscheme ctor moves it in
      SecureBinaryData chaincode = root135->getChaincode();
      auto derScheme = make_shared<DerivationScheme_ArmoryLegacy>(
         chaincode);

      if (!root135->hasPrivateKey())
      {
         //WO
         firstAsset = derScheme->computeNextPublicEntry(
            root135->getPubKey()->getUncompressedKey(),
            full_account_id, 0);
      }
      else
      {
         //full wallet
         ReentrantLock lock(decrData.get());
         const auto& privRootRef = decrData->getDecryptedPrivateData(
            root135->getPrivKey());

         firstAsset = derScheme->computeNextPrivateEntry(
            decrData,
            privRootRef, move(cipher),
            full_account_id, 0);
      }

      //instantiate account and set first entry
      auto asset_account = make_shared<AssetAccountData>(
         AssetAccountTypeEnum_Plain,
         asset_account_id, ID_,
         //no root asset for legacy derivation scheme, using first entry instead
         nullptr, derScheme, dbName_);
      asset_account->assets_.insert(make_pair(0, firstAsset));

      //add the asset account
      addAccount(asset_account);

      break;
   }

   case AccountTypeEnum_BIP32:
   case AccountTypeEnum_BIP32_Salted:
   {
      auto accBip32 = dynamic_pointer_cast<AccountType_BIP32>(accType);
      if (accBip32 == nullptr)
         throw AccountException("unexpected account type");

      //set ID, used after the switch statement
      ID_ = accBip32->getAccountID();

      //grab derivation tree, generate node roots
      const auto& derTree = accBip32->getDerivationTree();
      auto walletRootBip32 = 
         dynamic_pointer_cast<AssetEntry_BIP32Root>(getRootLbd());
      
      ReentrantLock lock(decrData.get());
      auto nodeRoots = derTree.resolveNodeRoots(decrData, walletRootBip32);

      for (const auto& nodeRoot : nodeRoots)
      {
         if (nodeRoot.b58Root.empty())
            throw AccountException("[make_new] skipped path");

         unique_ptr<Cipher> cipher_copy;
         if (cipher != nullptr)
            cipher_copy = cipher->getCopy();

         auto root_obj = createRootAsset(accBip32,
            nodeRoot, move(cipher_copy));

         //derivation scheme object
         shared_ptr<DerivationScheme_BIP32> derScheme = nullptr;
         if (accType->type() == AccountTypeEnum_BIP32_Salted)
         {
            auto accSalted = 
               dynamic_pointer_cast<AccountType_BIP32_Salted>(accType);
            if (accSalted == nullptr)
               throw AccountException("unexpected account type");

            if (accSalted->getSalt().getSize() != 32)
               throw AccountException("invalid salt len");

            auto chaincode = root_obj->getChaincode();
            auto salt = accSalted->getSalt();
            derScheme = 
               make_shared<DerivationScheme_BIP32_Salted>(
                  salt, chaincode, 
                  root_obj->getDepth(), root_obj->getLeafID());
         }

         //create and add the asset account
         auto account_obj = createNewAccount(root_obj, derScheme);
         addAccount(account_obj);
      }

      break;
   }

   case AccountTypeEnum_ECDH:
   {
      auto accEcdh = dynamic_pointer_cast<AccountType_ECDH>(accType);
      if (accEcdh == nullptr)
         throw AccountException("unexpected account type");

      ID_ = accEcdh->getAccountID();

      //ids
      auto accountID = ID_;
      accountID.append(accEcdh->getOuterAccountID());

      //root asset
      shared_ptr<AssetEntry_Single> rootAsset;
      if (accEcdh->isWatchingOnly())
      {
         //WO
         auto pubkeyCopy = accEcdh->getPubKey();
         rootAsset = make_shared<AssetEntry_Single>(
            -1, accountID,
            pubkeyCopy, nullptr);
      }
      else
      {
         //full wallet
         auto pubkey = accEcdh->getPubKey();
         if (pubkey.getSize() == 0)
         {
            auto&& pubkey_unc =
               CryptoECDSA().ComputePublicKey(accEcdh->getPrivKey());
            pubkey = move(CryptoECDSA().CompressPoint(pubkey_unc));
         }

         ReentrantLock lock(decrData.get());

         //encrypt private root
         auto&& cipher_copy = cipher->getCopy();
         auto&& encrypted_root =
            decrData->encryptData(cipher_copy.get(), accEcdh->getPrivKey());

         //create assets
         auto privKeyID = accountID;
         privKeyID.append(WRITE_UINT32_LE(UINT32_MAX));
         auto priv_asset = make_shared<Asset_PrivateKey>(
            privKeyID, encrypted_root, move(cipher_copy));
         rootAsset = make_shared<AssetEntry_Single>(
            -1, accountID,
            pubkey, priv_asset);
      }

      //derivation scheme
      auto derScheme = make_shared<DerivationScheme_ECDH>();

      //account
      auto assetAccount = make_shared<AssetAccountData>(
         AssetAccountTypeEnum_ECDH,
         accEcdh->getOuterAccountID(), ID_,
         rootAsset, derScheme, dbName_);

      addAccount(assetAccount);
      break;
   }

   default:
      throw AccountException("unknown account type");
   }

   //set the address types
   addressTypes_ = accType->getAddressTypes();

   //set default address type
   defaultAddressEntryType_ = accType->getDefaultAddressEntryType();

   //set inner and outer accounts
   outerAccount_ = accType->getOuterAccountID();
   innerAccount_ = accType->getInnerAccountID();

   //sanity checks
   if (accountDataMap_.empty())
   {
      throw AccountException("[make_new] address account has no"
         " asset account!");
   }

   //check outer account, set default if empty
   if (outerAccount_.empty())
   {
      outerAccount_ = accountDataMap_.begin()->first;
      LOGWARN << "emtpy outer account id, defaulting to " <<
         outerAccount_.toHexStr();
   }
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::reset()
{
   outerAccount_.clear();
   innerAccount_.clear();

   accountDataMap_.clear();
   addressTypes_.clear();
   addressHashes_.clear();
   ID_.clear();

   addresses_.clear();
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::commit(shared_ptr<WalletDBInterface> iface)
{
   if (iface == nullptr)
      throw AccountException("commit: null iface");

   //id as key
   BinaryWriter bwKey;
   bwKey.put_uint8_t(ADDRESS_ACCOUNT_PREFIX);
   bwKey.put_BinaryData(ID_);

   //data
   BinaryWriter bwData;

   //outer and inner account
   bwData.put_var_int(outerAccount_.getSize());
   bwData.put_BinaryData(outerAccount_);

   bwData.put_var_int(innerAccount_.getSize());
   bwData.put_BinaryData(innerAccount_);

   //address type set
   bwData.put_var_int(addressTypes_.size());

   for (auto& addrType : addressTypes_)
      bwData.put_uint32_t(addrType);

   //default address type
   bwData.put_uint32_t(defaultAddressEntryType_);

   //asset accounts count
   bwData.put_var_int(accountDataMap_.size());

   auto uniqueTx = iface->beginWriteTransaction(dbName_);
   shared_ptr<DBIfaceTransaction> sharedTx(move(uniqueTx));

   //asset accounts
   for (auto& accDataPair : accountDataMap_)
   {
      shared_ptr<AssetAccount> aaPtr;
      switch (accDataPair.second->type_)
      {
      case AssetAccountTypeEnum_Plain:
      {
         aaPtr = make_shared<AssetAccount>(
            accDataPair.second);
         break;
      }

      case AssetAccountTypeEnum_ECDH:
      {
         aaPtr = make_shared<AssetAccount_ECDH>(
            accDataPair.second);
         break;
      }

      default:
         throw AccountException("invalid asset account type");
      }

      auto&& assetAccountID = aaPtr->getFullID();
      bwData.put_var_int(assetAccountID.getSize());
      bwData.put_BinaryData(assetAccountID);

      aaPtr->commit(iface);
   }

   //commit address account data to disk
   sharedTx->insert(bwKey.getData(), bwData.getData());

   //commit instantiated address types
   for (auto& addrPair : addresses_)
      writeAddressType(sharedTx, addrPair.first, addrPair.second);
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::addAccount(shared_ptr<AssetAccount> account)
{
   addAccount(account->data_);
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::addAccount(std::shared_ptr<AssetAccountData> accPtr)
{
   auto& accID = accPtr->id_;
   if (accID.getSize() != 4)
      throw AccountException("invalid account id length");

   auto insertPair = accountDataMap_.emplace(accID, accPtr);
   if (!insertPair.second)
      throw AccountException("already have this asset account");
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::readFromDisk(
   shared_ptr<WalletDBInterface> iface, const BinaryData& key)
{
   //sanity checks
   if (key.getSize() == 0)
      throw AccountException("empty AddressAccount key");

   if (key.getPtr()[0] != ADDRESS_ACCOUNT_PREFIX)
      throw AccountException("unexpected key prefix for AddressAccount");

   if (iface == nullptr || dbName_.size() == 0)
      throw AccountException("unintialized AddressAccount object");

   //wipe object prior to loading from disk
   reset();

   //get data from disk  
   auto&& tx = iface->beginReadTransaction(dbName_);
   auto&& diskDataRef = tx->getDataRef(key);
   BinaryRefReader brr(diskDataRef);

   //outer and inner accounts
   size_t len, count;
   
   len = brr.get_var_int();
   outerAccount_ = brr.get_BinaryData(len);

   len = brr.get_var_int();
   innerAccount_ = brr.get_BinaryData(len);

   //address type set
   count = brr.get_var_int();
   for (unsigned i = 0; i < count; i++)
      addressTypes_.insert(AddressEntryType(brr.get_uint32_t()));

   //default address type
   defaultAddressEntryType_ = AddressEntryType(brr.get_uint32_t());

   //asset accounts
   count = brr.get_var_int();

   for (unsigned i = 0; i < count; i++)
   {
      len = brr.get_var_int();
      BinaryWriter bw_asset_key(1 + len);
      bw_asset_key.put_uint8_t(ASSET_ACCOUNT_PREFIX);
      bw_asset_key.put_BinaryData(brr.get_BinaryData(len));

      auto accData = AssetAccount::loadFromDisk(
         bw_asset_key.getData(), iface, dbName_);
      accountDataMap_.emplace(accData->id_, move(accData));
   }

   ID_ = key.getSliceCopy(1, key.getSize() - 1);

   //instantiated address types
   BinaryWriter bwKey;
   bwKey.put_uint8_t(ADDRESS_TYPE_PREFIX);
   bwKey.put_BinaryData(getID());
   auto keyBdr = bwKey.getDataRef();

   auto dbIter = tx->getIterator();
   dbIter->seek(bwKey.getData());
   while (dbIter->isValid())
   {
      auto&& key = dbIter->key();
      if (!key.startsWith(keyBdr))
         break;

      if (key.getSize() != 13)
      {
         LOGWARN << "unexpected address entry type key size!";
         dbIter->advance();
         continue;
      }

      auto&& data = dbIter->value();
      if (data.getSize() != 4)
      {
         LOGWARN << "unexpected address entry type val size!";
         dbIter->advance();
         continue;
      }

      auto aeType = AddressEntryType(*(uint32_t*)data.getPtr());
      auto assetID = key.getSliceCopy(1, 12);
      addresses_.insert(make_pair(assetID, aeType));
      
      dbIter->advance();
   }
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::extendPublicChain(
   std::shared_ptr<WalletDBInterface> iface, unsigned count)
{
   for (auto& accDataPair : accountDataMap_)
   {
      auto accountPtr = getAccountForID(accDataPair.first);
      accountPtr->extendPublicChain(iface, count);
   }
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::extendPublicChain(
   std::shared_ptr<WalletDBInterface> iface,
   const BinaryData& id, unsigned count)
{
   auto accountPtr = getAccountForID(id);
   accountPtr->extendPublicChain(iface, count);
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::extendPublicChainToIndex(
   std::shared_ptr<WalletDBInterface> iface,
   const BinaryData& accountID, unsigned index)
{
   auto accountPtr = getAccountForID(accountID);
   accountPtr->extendPublicChainToIndex(iface, index);
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::extendPrivateChain(
   std::shared_ptr<WalletDBInterface> iface,
   shared_ptr<DecryptedDataContainer> ddc,
   unsigned count)
{
   for (auto& accDataPair : accountDataMap_)
   {
      auto accountPtr = getAccountForID(accDataPair.first);
      accountPtr->extendPrivateChain(iface, ddc, count);
   }
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::extendPrivateChainToIndex(
   std::shared_ptr<WalletDBInterface> iface,
   shared_ptr<DecryptedDataContainer> ddc,
   const BinaryData& accountID, unsigned count)
{
   auto account = getAccountForID(accountID);
   account->extendPrivateChainToIndex(iface, ddc, count);
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AddressEntry> AddressAccount::getNewAddress(
   std::shared_ptr<WalletDBInterface> iface, AddressEntryType aeType)
{
   if (outerAccount_.empty())
      throw AccountException("no currently active asset account");

   return getNewAddress(iface, outerAccount_, aeType);
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AddressEntry> AddressAccount::getNewAddress(
   std::shared_ptr<WalletDBInterface> iface,
   const BinaryData& account, AddressEntryType aeType)
{
   if (aeType == AddressEntryType_Default)
      aeType = defaultAddressEntryType_;

   auto aeIter = addressTypes_.find(aeType);
   if (aeIter == addressTypes_.end())
      throw AccountException("invalid address type for this account");


   auto accountPtr = getAccountForID(account);
   auto assetPtr = accountPtr->getNewAsset(iface);
   auto addrPtr = AddressEntry::instantiate(assetPtr, aeType);
   
   //keep track of the address type for this asset if it doesnt use the 
   //account default
   if (aeType != defaultAddressEntryType_)
   {
      //update on disk
      updateInstantiatedAddressType(iface, addrPtr);
   }

   return addrPtr;
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AddressEntry> AddressAccount::getNewChangeAddress(
   std::shared_ptr<WalletDBInterface> iface, AddressEntryType aeType)
{
   if (innerAccount_.empty())
      throw AccountException("no currently active asset account");

   return getNewAddress(iface, innerAccount_, aeType);
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AddressEntry> AddressAccount::peekNextChangeAddress(
   std::shared_ptr<WalletDBInterface> iface, AddressEntryType aeType)
{
   if (aeType == AddressEntryType_Default)
      aeType = defaultAddressEntryType_;

   auto aeIter = addressTypes_.find(aeType);
   if (aeIter == addressTypes_.end())
      throw AccountException("invalid address type for this account");

   auto accountPtr = getAccountForID(innerAccount_);
   auto assetPtr = accountPtr->getNewAsset(iface);
   auto addrPtr = AddressEntry::instantiate(assetPtr, aeType);
   
   return addrPtr;
}

////////////////////////////////////////////////////////////////////////////////
bool AddressAccount::hasAddressType(AddressEntryType aeType)
{
   if (aeType == AddressEntryType_Default)
      return true;

   auto iter = addressTypes_.find(aeType);
   return iter != addressTypes_.end();
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AssetEntry> AddressAccount::getAssetForID(const BinaryData& ID) const
{
   if (ID.getSize() != 8)
      throw AccountException("invalid asset ID");

   auto accID = ID.getSliceRef(0, 4);
   auto accountPtr = getAccountForID(accID);

   auto assetID = ID.getSliceRef(4, ID.getSize() - 4);
   return accountPtr->getAssetForID(assetID);
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AssetEntry> AddressAccount::getAssetForID(unsigned ID, 
   bool outer) const
{
   BinaryDataRef accountID(outerAccount_);
   if (!outer)
      accountID.setRef(innerAccount_);

   auto accountPtr = getAccountForID(accountID);
   return accountPtr->getAssetForID(WRITE_UINT32_BE(ID));
}

////////////////////////////////////////////////////////////////////////////////
const pair<BinaryData, AddressEntryType>& 
   AddressAccount::getAssetIDPairForAddr(const BinaryData& scrAddr)
{
   updateAddressHashMap();

   auto iter = addressHashes_.find(scrAddr);
   if (iter == addressHashes_.end())
      throw AccountException("unknown scrAddr");

   return iter->second;
}

////////////////////////////////////////////////////////////////////////////////
const pair<BinaryData, AddressEntryType>& 
   AddressAccount::getAssetIDPairForAddrUnprefixed(const BinaryData& scrAddr)
{
   updateAddressHashMap();

   auto addressTypeSet = getAddressTypeSet();
   set<uint8_t> usedPrefixes;
   for (auto& addrType : addressTypeSet)
   {
      BinaryWriter bw;
      auto prefixByte = AddressEntry::getPrefixByte(addrType);
      auto insertIter = usedPrefixes.insert(prefixByte);
      if (!insertIter.second)
         continue;

      bw.put_uint8_t(prefixByte);
      bw.put_BinaryData(scrAddr);

      auto& addrData = bw.getData();
      auto iter = addressHashes_.find(addrData);
      if (iter == addressHashes_.end())
         continue;

      return iter->second;
   }

   throw AccountException("unknown scrAddr");
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::updateAddressHashMap()
{
   ReentrantLock lock(this);

   for (auto accountDataPair : accountDataMap_)
   {
      auto accountPtr = getAccountForID(accountDataPair.first);
      auto& hashMap = accountPtr->getAddressHashMap(addressTypes_);
      if (hashMap.size() == 0)
         continue;

      map<BinaryData, map<AddressEntryType, BinaryData>>::const_iterator hashMapIter;

      auto idIter = topHashedAssetId_.find(accountDataPair.first);
      if (idIter == topHashedAssetId_.end())
      {
         hashMapIter = hashMap.begin();
      }
      else
      {
         hashMapIter = hashMap.find(idIter->second);
         ++hashMapIter;

         if (hashMapIter == hashMap.end())
            continue;
      }

      while (hashMapIter != hashMap.end())
      {
         for (auto& hash : hashMapIter->second)
         {
            auto&& inner_pair = make_pair(hashMapIter->first, hash.first);
            auto&& outer_pair = make_pair(hash.second, move(inner_pair));
            addressHashes_.emplace(outer_pair);
         }

         ++hashMapIter;
      }

      topHashedAssetId_[accountDataPair.first] = hashMap.rbegin()->first;
   }
}

////////////////////////////////////////////////////////////////////////////////
const map<BinaryData, pair<BinaryData, AddressEntryType>>& 
   AddressAccount::getAddressHashMap()
{
   updateAddressHashMap();
   return addressHashes_;
}

////////////////////////////////////////////////////////////////////////////////
const shared_ptr<AssetAccountData>& AddressAccount::getAccountDataForID(
   const BinaryData& id) const
{
   auto iter = accountDataMap_.find(id);
   if (iter == accountDataMap_.end())
      throw AccountException("invalid account ID");

   return iter->second;
}

////////////////////////////////////////////////////////////////////////////////
set<BinaryData> AddressAccount::getAccountIdSet(void) const
{
   set<BinaryData> result;
   for (const auto& accDataPair : accountDataMap_)
      result.emplace(accDataPair.first);

   return result;
}

////////////////////////////////////////////////////////////////////////////////
unique_ptr<AssetAccount> AddressAccount::getAccountForID(
   const BinaryData& id) const
{
   auto accData = getAccountDataForID(id);
   switch (accData->type_)
   {
      case AssetAccountTypeEnum_Plain:
         return make_unique<AssetAccount>(accData);

      case AssetAccountTypeEnum_ECDH:
         return make_unique<AssetAccount_ECDH>(accData);

      default:
         throw runtime_error("unknown asset account type");
   }
}

////////////////////////////////////////////////////////////////////////////////
unique_ptr<AssetAccount> AddressAccount::getOuterAccount() const
{
   return getAccountForID(getOuterAccountID());
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AssetEntry> AddressAccount::getOutterAssetForIndex(unsigned id) const
{
   auto account = getOuterAccount();
   return account->getAssetForIndex(id);
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AssetEntry> AddressAccount::getOutterAssetRoot() const
{
   auto account = getOuterAccount();
   return account->getRoot();
}

////////////////////////////////////////////////////////////////////////////////
AddressAccountPublicData AddressAccount::exportPublicData() const
{
   AddressAccountPublicData aapd;

   //id
   aapd.ID_ = ID_;

   //address
   aapd.defaultAddressEntryType_ = defaultAddressEntryType_;
   aapd.addressTypes_ = addressTypes_;
   aapd.addresses_ = addresses_;

   //account ids
   aapd.outerAccount_ = outerAccount_;
   aapd.innerAccount_ = innerAccount_;

   //asset accounts
   for (auto& assetAccPair : accountDataMap_)
   {
      auto accPtr = getAccountForID(assetAccPair.first);
      const auto& assetData = assetAccPair.second;
      if (assetData == nullptr)
         continue;

      /*
      Only check account root type if it has a root to begin with. Some
      accounts do not carry roots (e.g. Armory135 wallets)
      */
      shared_ptr<AssetEntry_Single> woRoot = nullptr;
      {
         auto rootSingle = dynamic_pointer_cast<AssetEntry_Single>(
            assetData->root_);

         if (rootSingle != nullptr)
            woRoot = rootSingle->getPublicCopy();
      }

      SecureBinaryData rootData;
      if (woRoot != nullptr)
         rootData = woRoot->serialize();

      SecureBinaryData derData;
      if (assetData->derScheme_ != nullptr)
         derData = assetData->derScheme_->serialize();

      AssetAccountPublicData assaPD {
         assetData->id_, assetData->parentId_,
         rootData, derData,
         accPtr->getHighestUsedIndex(), accPtr->getLastComputedIndex() };

      aapd.accountDataMap_.emplace(assetData->id_, move(assaPD));
   }

   return aapd;
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::importPublicData(const AddressAccountPublicData& aapd)
{
   //sanity check
   if (aapd.ID_ != ID_)
      throw AccountException("[importPublicData] ID mismatch");

   //synchronize address chains
   for (auto& assapd : aapd.accountDataMap_)
   {
      auto accPtr = getAccountForID(assapd.first);
      if (accPtr == nullptr)
         throw AccountException("[importPublicData] missing asset account");

      //do not allow rollbacks
      if (assapd.second.lastComputedIndex_ > accPtr->getLastComputedIndex())
      {
         accPtr->extendPublicChainToIndex(
            nullptr, assapd.second.lastComputedIndex_);
      }

      if (assapd.second.lastUsedIndex_ > accPtr->getHighestUsedIndex())
         accPtr->data_->lastUsedIndex_ = assapd.second.lastUsedIndex_;
   }

   //sync address set
   addresses_ = aapd.addresses_;

   //check the assets for addresses do exist

}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::updateInstantiatedAddressType(
   shared_ptr<WalletDBInterface> iface,
   shared_ptr<AddressEntry> addrPtr)
{
   /***
   AddressAccount keeps track instantiated address types with a simple
   key-val scheme:

   (ADDRESS_PREFIX|Asset's ID):(AddressEntry type)

   Addresses using the account's default type are not recorded. Their type is
   infered on load by AssetAccounts' highest used index and the lack of explicit
   type entry.
   ***/

   //sanity check
   if (addrPtr->getType() == AddressEntryType_Default)
      throw AccountException("invalid address entry type");

   updateInstantiatedAddressType(iface, addrPtr->getID(), addrPtr->getType());
}
  
////////////////////////////////////////////////////////////////////////////////
void AddressAccount::updateInstantiatedAddressType(
   shared_ptr<WalletDBInterface> iface,
   const BinaryData& id, AddressEntryType aeType)
{
   //TODO: sanity check
   /*if (aeType != AddressEntryType_Default)
   {
      auto typeIter = addressTypes_.find(aeType);
      if (typeIter == addressTypes_.end())
         throw AccountException("invalid address type");
   }*/

   auto iter = addresses_.find(id);
   if (iter != addresses_.end())
   {
      //skip if type entry already exist and new type matches old one
      if (iter->second == aeType)
         return;

      //delete entry if new type matches default account type
      if (aeType == defaultAddressEntryType_)
      {
         addresses_.erase(iter);
         eraseInstantiatedAddressType(iface, id);
         return;
      }
   }

   //otherwise write address type to disk
   addresses_[id] = aeType;
   writeAddressType(iface, id, aeType);
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::writeAddressType(shared_ptr<WalletDBInterface> iface,
   const BinaryData& id, AddressEntryType aeType)
{
   auto uniqueTx = iface->beginWriteTransaction(dbName_);
   shared_ptr<DBIfaceTransaction> sharedTx(move(uniqueTx));

   writeAddressType(sharedTx, id, aeType);
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::writeAddressType(shared_ptr<DBIfaceTransaction> tx,
   const BinaryData& id, AddressEntryType aeType)
{
   ReentrantLock lock(this);

   BinaryWriter bwKey;
   bwKey.put_uint8_t(ADDRESS_TYPE_PREFIX);
   bwKey.put_BinaryData(id);

   BinaryWriter bwData;
   bwData.put_uint32_t(aeType);

   tx->insert(bwKey.getData(), bwData.getData());
}

////////////////////////////////////////////////////////////////////////////////
void AddressAccount::eraseInstantiatedAddressType(
   std::shared_ptr<WalletDBInterface> iface, const BinaryData& id)
{
   if (iface == nullptr)
      throw AccountException("eraseInstantiatedAddressType: null iface");

   ReentrantLock lock(this);

   BinaryWriter bwKey;
   bwKey.put_uint8_t(ADDRESS_TYPE_PREFIX);
   bwKey.put_BinaryData(id);

   auto tx = iface->beginWriteTransaction(dbName_);
   tx->erase(bwKey.getData());
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AddressEntry> AddressAccount::getAddressEntryForID(
   const BinaryDataRef& ID) const
{
   //sanity check
   if (ID.getSize() != 12)
      throw AccountException("getAddressEntryForID: invalid asset id");

   //get the asset account
   auto accIDRef = ID.getSliceRef(4, 4);
   AssetAccount account(getAccountDataForID(accIDRef));

   //does this ID exist?
   BinaryRefReader brr(ID);
   brr.advance(8);
   auto id_int = brr.get_uint32_t(BE);

   if (id_int > account.getHighestUsedIndex())
      throw UnrequestedAddressException();

   AddressEntryType aeType = defaultAddressEntryType_;
   //is there an address entry with this ID?
   auto addrIter = addresses_.find(ID);
   if (addrIter != addresses_.end())
      aeType = addrIter->second;

   auto assetPtr = account.getAssetForIndex(id_int);
   auto addrPtr = AddressEntry::instantiate(assetPtr, aeType);
   return addrPtr;
}

////////////////////////////////////////////////////////////////////////////////
map<BinaryData, shared_ptr<AddressEntry>> AddressAccount::getUsedAddressMap()
   const
{
   /***
   Expensive call, as addresses are built on the fly
   ***/

   map<BinaryData, shared_ptr<AddressEntry>> result;

   for (auto& account : accountDataMap_)
   {
      const AssetAccount aa(account.second);

      auto usedIndex = aa.getHighestUsedIndex();
      if (usedIndex == UINT32_MAX)
         continue;


      for (unsigned i = 0; i <= usedIndex; i++)
      {
         auto assetPtr = aa.getAssetForIndex(i);
         auto& assetID = assetPtr->getID();

         shared_ptr<AddressEntry> addrPtr;
         auto iter = addresses_.find(assetID);
         if (iter == addresses_.end())
            addrPtr = AddressEntry::instantiate(assetPtr, defaultAddressEntryType_);
         else
            addrPtr = AddressEntry::instantiate(assetPtr, iter->second);

         result.insert(make_pair(assetID, addrPtr));
      }
   }

   return result;
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<Asset_PrivateKey> AddressAccount::fillPrivateKey(
   shared_ptr<WalletDBInterface> iface,
   shared_ptr<DecryptedDataContainer> ddc,
   const BinaryData& id)
{
   if (id.getSize() != 12)
      throw AccountException("invalid asset id");

   auto accID = id.getSliceRef(4, 4);
   auto accountPtr = getAccountForID(accID);

   return accountPtr->fillPrivateKey(iface, ddc, id);
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AssetEntry_BIP32Root> AddressAccount::getBip32RootForAssetId(
   const BinaryData& assetId) const
{
   //sanity check
   if (assetId.getSize() != 12)
      throw AccountException("invalid asset id");

   //get the asset account
   auto accID = assetId.getSliceRef(4, 4);
   const auto& acc = getAccountDataForID(accID);

   //grab the account's root
   auto root = acc->root_;

   //is it bip32?
   auto rootBip32 = dynamic_pointer_cast<AssetEntry_BIP32Root>(root);
   if (rootBip32 == nullptr)
      throw AccountException("account isn't bip32");

   return rootBip32;
}

////////////////////////////////////////////////////////////////////////////////
bool AddressAccount::hasBip32Path(
   const ArmorySigner::BIP32_AssetPath& path) const
{
   //look for an account which root's path matches that of our desired path
   for (const auto& accountPair : accountDataMap_)
   {
      auto root = accountPair.second->root_;
      auto rootBip32 = dynamic_pointer_cast<AssetEntry_BIP32Root>(root);
      if (rootBip32 == nullptr)
         continue;

      auto rootPath = rootBip32->getDerivationPath();
      auto assetPath = path.getDerivationPathFromSeed();
      if (rootPath.empty() || 
         (rootPath.size() > assetPath.size()))
      {
         continue;
      }

      if (rootBip32->getSeedFingerprint(true) != path.getSeedFingerprint())
         return false;

      bool match = true;
      for (unsigned i=0; i<rootPath.size(); i++)
      {
         if (rootPath[i] != assetPath[i])
         {
            match = false;
            break;
         }
      }

      if (match)
         return true;
   }

   return false;
}