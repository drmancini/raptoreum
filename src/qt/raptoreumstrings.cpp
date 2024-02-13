

#include <QtGlobal>

// Automatically generated by extract_strings_qt.py
#ifdef __GNUC__
#define UNUSED __attribute__((unused))
#else
#define UNUSED
#endif
static const char UNUSED *raptoreum_strings[] = {
QT_TRANSLATE_NOOP("raptoreum-core", "The %s developers"),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"%s corrupt. Try using the wallet tool raptoreum-wallet to salvage or "
"restoring a backup."),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"%s file contains all private keys from this wallet. Do not share it with "
"anyone!"),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"%s uses exact denominated amounts to send funds, you might simply need to "
"mix some more coins."),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"-maxtxfee is set very high! Fees this large could be paid on a single "
"transaction."),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"-zapwallettxes has been removed. If you are attempting to remove a stuck "
"transaction from your wallet, please use abandontransaction instead."),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Can not generate a change-address key. Private keys are disabled for this "
"wallet."),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Cannot obtain a lock on data directory %s. %s is probably already running."),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Cannot provide specific connections and have addrman find outgoing "
"connections at the same."),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Distributed under the MIT software license, see the accompanying file %s or "
"%s"),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Error loading %s: You can't enable HD on an already existing non-HD wallet"),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Error reading %s! All keys read correctly, but transaction data or address "
"book entries might be missing or incorrect."),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Error: Listening for incoming connections failed (listen returned error %s)"),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Failed to create backup, file already exists! This could happen if you "
"restarted wallet in less than 60 seconds. You can continue if you are ok "
"with this."),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Found unconfirmed denominated outputs, will wait till they confirm to "
"continue."),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Group outputs by address, selecting all or none, instead of selecting on a "
"per-output basis. Privacy is improved as an address is only used once "
"(unless someone sends to it after spending from it), but may result in "
"slightly higher fees as suboptimal coin selection may result due to the "
"added limitation (default: %u)"),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Incorrect or no devnet genesis block found. Wrong datadir for devnet "
"specified?"),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Invalid -socketevents ('%s') specified. Only these modes are supported: %s"),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Invalid amount for -maxtxfee=<amount>: '%s' (must be at least the minrelay "
"fee of %s to prevent stuck transactions)"),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Make sure to encrypt your wallet and delete all non-encrypted backups after "
"you have verified that the wallet works!"),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Please check that your computer's date and time are correct! If your clock "
"is wrong, %s will not work properly."),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Please contribute if you find %s useful. Visit %s for further information "
"about the software."),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Prune configured below the minimum of %d MiB.  Please use a higher number."),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Prune: last wallet synchronisation goes beyond pruned data. You need to -"
"reindex (download the whole blockchain again in case of pruned node)"),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"The block database contains a block which appears to be from the future. "
"This may only be due to your computer's date and time being set incorrectly. "
"Only rebuild the block database if you are sure that your computer's date "
"and time are correct."),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"The transaction amount is too small to send after the fee has been deducted"),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"This is a pre-release test build - use at your own risk - do not use for "
"mining or merchant applications"),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"This is the transaction fee you may discard if change is smaller than dust "
"at this level"),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"This is the transaction fee you may pay when fee estimates are not available."),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"This product includes software developed by the OpenSSL Project for use in "
"the OpenSSL Toolkit %s and cryptographic software written by Eric Young and "
"UPnP software written by Thomas Bernard."),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Total length of network version string (%i) exceeds maximum length (%i). "
"Reduce the number or size of uacomments."),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Transaction index can't be disabled with governance validation enabled. "
"Either start with -disablegovernance command line switch or enable "
"transaction index."),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Unable to rerply blocks. You will need tpo rebuild the database using -"
"reindex-chainstate."),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"WARNING! Failed to replenish keypool, please unlock your wallet to do so."),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Wallet is locked, can't replenish keypool! Automatic backups and mixing are "
"disabled, please unlock your wallet to replenish keypool."),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Warning: Private keys detected in wallet {%s} with disabled private keys"),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Warning: The Network does not appear to fully agree! Some miners addear to "
"experiencing issues."),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Warning: Unknown block versions being mined! It's possible unknown rules are "
"in effect"),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"Warning: We do not appear to fully agree with our peers! You may need to "
"upgrade, or other nodes may need to upgrade."),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"You need to rebuild the database using -reindex to change -timestampindex"),
QT_TRANSLATE_NOOP("raptoreum-core", ""
"You need to rebuild the database using -reindex to go back to unpruned "
"mode.  This will redownload the entire blockchain"),
QT_TRANSLATE_NOOP("raptoreum-core", "%d of last 100 blocks have unexpected version"),
QT_TRANSLATE_NOOP("raptoreum-core", "%s can't be lower than %s"),
QT_TRANSLATE_NOOP("raptoreum-core", "%s failed"),
QT_TRANSLATE_NOOP("raptoreum-core", "%s is idle."),
QT_TRANSLATE_NOOP("raptoreum-core", "%s is not a valid backup folder!"),
QT_TRANSLATE_NOOP("raptoreum-core", "%s is set very high!"),
QT_TRANSLATE_NOOP("raptoreum-core", "%s request incomplete: %s"),
QT_TRANSLATE_NOOP("raptoreum-core", "-devnet can only be specified once"),
QT_TRANSLATE_NOOP("raptoreum-core", "-maxmempool must be at least %d MB"),
QT_TRANSLATE_NOOP("raptoreum-core", "-port must be specified when -devnet and -listen are specified"),
QT_TRANSLATE_NOOP("raptoreum-core", "-rpcport must be specified when -devnet and -server are specified"),
QT_TRANSLATE_NOOP("raptoreum-core", "Already have that input."),
QT_TRANSLATE_NOOP("raptoreum-core", "Asset Transfer amounts must be greater than 0"),
QT_TRANSLATE_NOOP("raptoreum-core", "Asset owner key not in wallet"),
QT_TRANSLATE_NOOP("raptoreum-core", "Automatic backups disabled"),
QT_TRANSLATE_NOOP("raptoreum-core", "Can't find random Smartnode."),
QT_TRANSLATE_NOOP("raptoreum-core", "Can't mix while sync in progress."),
QT_TRANSLATE_NOOP("raptoreum-core", "Can't mix: no compatible inputs found!"),
QT_TRANSLATE_NOOP("raptoreum-core", "Cannot downgrade wallet"),
QT_TRANSLATE_NOOP("raptoreum-core", "Cannot resolve -%s address: '%s'"),
QT_TRANSLATE_NOOP("raptoreum-core", "Cannot write to data directory '%s'; check permissions."),
QT_TRANSLATE_NOOP("raptoreum-core", "Change index out of range"),
QT_TRANSLATE_NOOP("raptoreum-core", "Collateral not valid."),
QT_TRANSLATE_NOOP("raptoreum-core", "Config settings for %s only applied on %s network when in [%s] section."),
QT_TRANSLATE_NOOP("raptoreum-core", "Copyright (C)"),
QT_TRANSLATE_NOOP("raptoreum-core", "Corrupted block database detected"),
QT_TRANSLATE_NOOP("raptoreum-core", "Could not find asmap file %s"),
QT_TRANSLATE_NOOP("raptoreum-core", "Could not parse asmap file %s"),
QT_TRANSLATE_NOOP("raptoreum-core", "Do you want to rebuild the block database now?"),
QT_TRANSLATE_NOOP("raptoreum-core", "Done loading"),
QT_TRANSLATE_NOOP("raptoreum-core", "ERROR! Failed to create automatic backup"),
QT_TRANSLATE_NOOP("raptoreum-core", "Entries are full."),
QT_TRANSLATE_NOOP("raptoreum-core", "Entry exceeds maximum size."),
QT_TRANSLATE_NOOP("raptoreum-core", "Error initializing block database"),
QT_TRANSLATE_NOOP("raptoreum-core", "Error initializing wallet database environment %s!"),
QT_TRANSLATE_NOOP("raptoreum-core", "Error loading %s"),
QT_TRANSLATE_NOOP("raptoreum-core", "Error loading %s: Private keys can only be disabled during creation"),
QT_TRANSLATE_NOOP("raptoreum-core", "Error loading %s: Wallet corrupted"),
QT_TRANSLATE_NOOP("raptoreum-core", "Error loading %s: Wallet requires newer version of %s"),
QT_TRANSLATE_NOOP("raptoreum-core", "Error loading %s: You can't disable HD on an already existing HD wallet"),
QT_TRANSLATE_NOOP("raptoreum-core", "Error loading block database"),
QT_TRANSLATE_NOOP("raptoreum-core", "Error loading wallet %s. Duplicate -wallet filename specified."),
QT_TRANSLATE_NOOP("raptoreum-core", "Error opening block database"),
QT_TRANSLATE_NOOP("raptoreum-core", "Error reading from database, shutting down."),
QT_TRANSLATE_NOOP("raptoreum-core", "Error upgrading chainstate database"),
QT_TRANSLATE_NOOP("raptoreum-core", "Error upgrading evo database"),
QT_TRANSLATE_NOOP("raptoreum-core", "Error"),
QT_TRANSLATE_NOOP("raptoreum-core", "Error: A fatal internal error occurred, see debug.log for details"),
QT_TRANSLATE_NOOP("raptoreum-core", "Error: Disk space is low for %s"),
QT_TRANSLATE_NOOP("raptoreum-core", "Error: Disk space is low!"),
QT_TRANSLATE_NOOP("raptoreum-core", "Error: failed to add socket to epollfd (epoll_ctl returned error %s)"),
QT_TRANSLATE_NOOP("raptoreum-core", "Error: failed to add socket to kqueuefd (kevent returned error %s)"),
QT_TRANSLATE_NOOP("raptoreum-core", "Exceeded max tries."),
QT_TRANSLATE_NOOP("raptoreum-core", "Failed to clear fulfilled requests cache at"),
QT_TRANSLATE_NOOP("raptoreum-core", "Failed to clear governance cache at"),
QT_TRANSLATE_NOOP("raptoreum-core", "Failed to clear smartnode cache at"),
QT_TRANSLATE_NOOP("raptoreum-core", "Failed to commit EvoDB"),
QT_TRANSLATE_NOOP("raptoreum-core", "Failed to create backup %s!"),
QT_TRANSLATE_NOOP("raptoreum-core", "Failed to create backup, error: %s"),
QT_TRANSLATE_NOOP("raptoreum-core", "Failed to delete backup, error: %s"),
QT_TRANSLATE_NOOP("raptoreum-core", "Failed to find mixing queue to join"),
QT_TRANSLATE_NOOP("raptoreum-core", "Failed to get root metadata"),
QT_TRANSLATE_NOOP("raptoreum-core", "Failed to listen on any port. Use -listen=0 if you want this."),
QT_TRANSLATE_NOOP("raptoreum-core", "Failed to load Assets Database"),
QT_TRANSLATE_NOOP("raptoreum-core", "Failed to load POW cache from"),
QT_TRANSLATE_NOOP("raptoreum-core", "Failed to load fulfilled requests cache from"),
QT_TRANSLATE_NOOP("raptoreum-core", "Failed to load governance cache from"),
QT_TRANSLATE_NOOP("raptoreum-core", "Failed to load smartnode cache from"),
QT_TRANSLATE_NOOP("raptoreum-core", "Failed to load sporks cache from"),
QT_TRANSLATE_NOOP("raptoreum-core", "Failed to rescan the wallet during initialization"),
QT_TRANSLATE_NOOP("raptoreum-core", "Failed to sign special tx"),
QT_TRANSLATE_NOOP("raptoreum-core", "Failed to start a new mixing queue"),
QT_TRANSLATE_NOOP("raptoreum-core", "Found enough users, signing ( waiting %s )"),
QT_TRANSLATE_NOOP("raptoreum-core", "Found enough users, signing ..."),
QT_TRANSLATE_NOOP("raptoreum-core", "Importing..."),
QT_TRANSLATE_NOOP("raptoreum-core", "Incompatible mode."),
QT_TRANSLATE_NOOP("raptoreum-core", "Incompatible version."),
QT_TRANSLATE_NOOP("raptoreum-core", "Incorrect -rescan mode, falling back to default value"),
QT_TRANSLATE_NOOP("raptoreum-core", "Incorrect or no genesis block found. Wrong datadir for network?"),
QT_TRANSLATE_NOOP("raptoreum-core", "Information"),
QT_TRANSLATE_NOOP("raptoreum-core", "Initialization sanity check failed. %s is shutting down."),
QT_TRANSLATE_NOOP("raptoreum-core", "Input is not valid."),
QT_TRANSLATE_NOOP("raptoreum-core", "Inputs vs outputs size mismatch."),
QT_TRANSLATE_NOOP("raptoreum-core", "Insufficient Unique asset funds"),
QT_TRANSLATE_NOOP("raptoreum-core", "Insufficient asset funds"),
QT_TRANSLATE_NOOP("raptoreum-core", "Insufficient funds."),
QT_TRANSLATE_NOOP("raptoreum-core", "Invalid -onion address or hostname: '%s'"),
QT_TRANSLATE_NOOP("raptoreum-core", "Invalid -proxy address or hostname: '%s'"),
QT_TRANSLATE_NOOP("raptoreum-core", "Invalid amount for -%s=<amount>: '%s'"),
QT_TRANSLATE_NOOP("raptoreum-core", "Invalid amount for -discardfee=<amount>: '%s'"),
QT_TRANSLATE_NOOP("raptoreum-core", "Invalid amount for -fallbackfee=<amount>: '%s'"),
QT_TRANSLATE_NOOP("raptoreum-core", "Invalid amount for -paytxfee=<amount>: '%s' (must be at least %s)"),
QT_TRANSLATE_NOOP("raptoreum-core", "Invalid minimum number of spork signers specified with -minsporkkeys"),
QT_TRANSLATE_NOOP("raptoreum-core", "Invalid netmask specified in -whitelist: '%s'"),
QT_TRANSLATE_NOOP("raptoreum-core", "Invalid script detected."),
QT_TRANSLATE_NOOP("raptoreum-core", "Invalid smartnodeblsprivkey. Please see documentation."),
QT_TRANSLATE_NOOP("raptoreum-core", "Invalid spork address specified with -sporkaddr"),
QT_TRANSLATE_NOOP("raptoreum-core", "Keypool ran out, please call keypoolrefill first"),
QT_TRANSLATE_NOOP("raptoreum-core", "Last queue was created too recently."),
QT_TRANSLATE_NOOP("raptoreum-core", "Last successful action was too recent."),
QT_TRANSLATE_NOOP("raptoreum-core", "Loading P2P addresses..."),
QT_TRANSLATE_NOOP("raptoreum-core", "Loading POW cache..."),
QT_TRANSLATE_NOOP("raptoreum-core", "Loading banlist..."),
QT_TRANSLATE_NOOP("raptoreum-core", "Loading block index..."),
QT_TRANSLATE_NOOP("raptoreum-core", "Loading fulfilled requests cache..."),
QT_TRANSLATE_NOOP("raptoreum-core", "Loading governance cache..."),
QT_TRANSLATE_NOOP("raptoreum-core", "Loading smartnode cache..."),
QT_TRANSLATE_NOOP("raptoreum-core", "Loading sporks cache..."),
QT_TRANSLATE_NOOP("raptoreum-core", "Loading wallet... (%3.2f %%)"),
QT_TRANSLATE_NOOP("raptoreum-core", "Loading wallet..."),
QT_TRANSLATE_NOOP("raptoreum-core", "Lock is already in place."),
QT_TRANSLATE_NOOP("raptoreum-core", "Missing input transaction information."),
QT_TRANSLATE_NOOP("raptoreum-core", "Mixing in progress..."),
QT_TRANSLATE_NOOP("raptoreum-core", "Need to specify a port with -whitebind: '%s'"),
QT_TRANSLATE_NOOP("raptoreum-core", "No Smartnodes detected."),
QT_TRANSLATE_NOOP("raptoreum-core", "No compatible Smartnode found."),
QT_TRANSLATE_NOOP("raptoreum-core", "No errors detected."),
QT_TRANSLATE_NOOP("raptoreum-core", "No matching denominations found for mixing."),
QT_TRANSLATE_NOOP("raptoreum-core", "Not compatible with existing transactions."),
QT_TRANSLATE_NOOP("raptoreum-core", "Not enough file descriptors available."),
QT_TRANSLATE_NOOP("raptoreum-core", "Not enough funds to mix."),
QT_TRANSLATE_NOOP("raptoreum-core", "Not in the Smartnode list."),
QT_TRANSLATE_NOOP("raptoreum-core", "Prune cannot be configured with a negative value."),
QT_TRANSLATE_NOOP("raptoreum-core", "Prune mode is incompatible with -disablegovernance=false."),
QT_TRANSLATE_NOOP("raptoreum-core", "Prune mode is incompatible with -txindex."),
QT_TRANSLATE_NOOP("raptoreum-core", "Pruning blockstore..."),
QT_TRANSLATE_NOOP("raptoreum-core", "Reducing -maxconnections from %d to %d, because of system limitations."),
QT_TRANSLATE_NOOP("raptoreum-core", "Replaying blocks..."),
QT_TRANSLATE_NOOP("raptoreum-core", "Rescanning..."),
QT_TRANSLATE_NOOP("raptoreum-core", "Root asset key not in wallet"),
QT_TRANSLATE_NOOP("raptoreum-core", "Section [%s] is not recognized."),
QT_TRANSLATE_NOOP("raptoreum-core", "Session not complete!"),
QT_TRANSLATE_NOOP("raptoreum-core", "Session timed out."),
QT_TRANSLATE_NOOP("raptoreum-core", "Signing transaction failed"),
QT_TRANSLATE_NOOP("raptoreum-core", "Smartnode queue is full."),
QT_TRANSLATE_NOOP("raptoreum-core", "Smartnode:"),
QT_TRANSLATE_NOOP("raptoreum-core", "Specified -walletdir \"%s\" does not exist"),
QT_TRANSLATE_NOOP("raptoreum-core", "Specified -walletdir \"%s\" is a relative path"),
QT_TRANSLATE_NOOP("raptoreum-core", "Specified -walletdir \"%s\" is not a directory"),
QT_TRANSLATE_NOOP("raptoreum-core", "Specified blocks directory \"%s\" does not exist."),
QT_TRANSLATE_NOOP("raptoreum-core", "Starting network threads..."),
QT_TRANSLATE_NOOP("raptoreum-core", "Submitted to smartnode, waiting in queue %s"),
QT_TRANSLATE_NOOP("raptoreum-core", "Synchronization finished"),
QT_TRANSLATE_NOOP("raptoreum-core", "Synchronizing blockchain..."),
QT_TRANSLATE_NOOP("raptoreum-core", "Synchronizing governance objects..."),
QT_TRANSLATE_NOOP("raptoreum-core", "The source code is available from %s."),
QT_TRANSLATE_NOOP("raptoreum-core", "The specified config file %s does not exist\n"),
QT_TRANSLATE_NOOP("raptoreum-core", "The transaction amount is too small to pay the fee"),
QT_TRANSLATE_NOOP("raptoreum-core", "The wallet will avoid paying less than the minimum relay fee."),
QT_TRANSLATE_NOOP("raptoreum-core", "This is expected because you are running a pruned node."),
QT_TRANSLATE_NOOP("raptoreum-core", "This is experimental software."),
QT_TRANSLATE_NOOP("raptoreum-core", "This is the minimum transaction fee you pay on every transaction."),
QT_TRANSLATE_NOOP("raptoreum-core", "This is the transaction fee you will pay if you send a transaction."),
QT_TRANSLATE_NOOP("raptoreum-core", "Transaction amount too small"),
QT_TRANSLATE_NOOP("raptoreum-core", "Transaction amounts must not be negative"),
QT_TRANSLATE_NOOP("raptoreum-core", "Transaction created successfully."),
QT_TRANSLATE_NOOP("raptoreum-core", "Transaction fees are too high."),
QT_TRANSLATE_NOOP("raptoreum-core", "Transaction has too long of a mempool chain"),
QT_TRANSLATE_NOOP("raptoreum-core", "Transaction must have at least one recipient"),
QT_TRANSLATE_NOOP("raptoreum-core", "Transaction not valid."),
QT_TRANSLATE_NOOP("raptoreum-core", "Transaction too large for fee policy"),
QT_TRANSLATE_NOOP("raptoreum-core", "Transaction too large"),
QT_TRANSLATE_NOOP("raptoreum-core", "Trying to connect..."),
QT_TRANSLATE_NOOP("raptoreum-core", "Unable to bind to %s on this computer (bind returned error %s)"),
QT_TRANSLATE_NOOP("raptoreum-core", "Unable to bind to %s on this computer. %s is probably already running."),
QT_TRANSLATE_NOOP("raptoreum-core", "Unable to create PID file '%s': %s"),
QT_TRANSLATE_NOOP("raptoreum-core", "Unable to generate initial keys"),
QT_TRANSLATE_NOOP("raptoreum-core", "Unable to locate enough mixed funds for this transaction."),
QT_TRANSLATE_NOOP("raptoreum-core", "Unable to locate enough non-denominated funds for this transaction."),
QT_TRANSLATE_NOOP("raptoreum-core", "Unable to sign spork message, wrong key?"),
QT_TRANSLATE_NOOP("raptoreum-core", "Unable to start HTTP server. See debug log for details."),
QT_TRANSLATE_NOOP("raptoreum-core", "Unknown network specified in -onlynet: '%s'"),
QT_TRANSLATE_NOOP("raptoreum-core", "Unknown response."),
QT_TRANSLATE_NOOP("raptoreum-core", "Unknown state: id = %u"),
QT_TRANSLATE_NOOP("raptoreum-core", "Unsupported logging category %s=%s."),
QT_TRANSLATE_NOOP("raptoreum-core", "Upgrading UTXO database"),
QT_TRANSLATE_NOOP("raptoreum-core", "Upgrading txindex database"),
QT_TRANSLATE_NOOP("raptoreum-core", "User Agent comment (%s) contains unsafe characters."),
QT_TRANSLATE_NOOP("raptoreum-core", "Verifying blocks..."),
QT_TRANSLATE_NOOP("raptoreum-core", "Verifying wallet(s)..."),
QT_TRANSLATE_NOOP("raptoreum-core", "Very low number of keys left: %d"),
QT_TRANSLATE_NOOP("raptoreum-core", "Wallet is locked."),
QT_TRANSLATE_NOOP("raptoreum-core", "Wallet needed to be rewritten: restart %s to complete"),
QT_TRANSLATE_NOOP("raptoreum-core", "Warning"),
QT_TRANSLATE_NOOP("raptoreum-core", "Warning: can't use %s and %s together, will prefer %s"),
QT_TRANSLATE_NOOP("raptoreum-core", "Warning: incorrect parameter %s, path must exist! Using default path."),
QT_TRANSLATE_NOOP("raptoreum-core", "Warning: unknown new rules activated (versionbit %i)"),
QT_TRANSLATE_NOOP("raptoreum-core", "Wasn't able to create wallet backup folder %s!"),
QT_TRANSLATE_NOOP("raptoreum-core", "Will retry..."),
QT_TRANSLATE_NOOP("raptoreum-core", "You are starting with governance validation disabled."),
QT_TRANSLATE_NOOP("raptoreum-core", "You can not disable governance validation on a smartnode."),
QT_TRANSLATE_NOOP("raptoreum-core", "You can not start a smartnode with wallet enabled."),
QT_TRANSLATE_NOOP("raptoreum-core", "You need to rebuild the database using -reindex to change -addressindex"),
QT_TRANSLATE_NOOP("raptoreum-core", "You need to rebuild the database using -reindex to change -assetindex"),
QT_TRANSLATE_NOOP("raptoreum-core", "You need to rebuild the database using -reindex to change -futureindex"),
QT_TRANSLATE_NOOP("raptoreum-core", "You need to rebuild the database using -reindex to change -spentindex"),
QT_TRANSLATE_NOOP("raptoreum-core", "Your entries added successfully."),
QT_TRANSLATE_NOOP("raptoreum-core", "no mixing available."),
QT_TRANSLATE_NOOP("raptoreum-core", "see debug.log for details."),
};
