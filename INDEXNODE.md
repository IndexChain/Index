Indexnode Build Instructions and Notes
=============================
 - Version 0.1.6
 - Date: 14 December 2017
 - More detailed guide available here: https://indexchain.org/index-indexnode-setup-guide/

Prerequisites
-------------
 - Ubuntu 16.04+
 - Libraries to build from Index source
 - Port **7082** is open

Step 1. Build
----------------------
**1.1.**  Check out from source:

    git clone https://github.com/IndexChain/Index

**1.2.**  See [README.md](README.md) for instructions on building.

Step 2. (Optional - only if firewall is running). Open port 7082
----------------------
**2.1.**  Run:

    sudo ufw allow 7082
    sudo ufw default allow outgoing
    sudo ufw enable

Step 3. First run on your Local Wallet
----------------------
**3.0.**  Go to the checked out folder

    cd Index

**3.1.**  Start daemon in testnet mode:

    ./src/indexd -daemon -server -testnet

**3.2.**  Generate indexnodeprivkey:

    ./src/index-cli indexnode genkey

(Store this key)

**3.3.**  Get wallet address:

    ./src/index-cli getaccountaddress 0

**3.4.**  Send to received address **exactly 1000 IDX** in **1 transaction**. Wait for 15 confirmations.

**3.5.**  Stop daemon:

    ./src/index-cli stop

Step 4. In your VPS where you are hosting your Indexnode. Update config files
----------------------
**4.1.**  Create file **index.conf** (in folder **~/.index**)

    rpcuser=username
    rpcpassword=password
    rpcallowip=127.0.0.1
    debug=1
    txindex=1
    daemon=1
    server=1
    listen=1
    maxconnections=24
    indexnode=1
    indexnodeprivkey=XXXXXXXXXXXXXXXXX  ## Replace with your indexnode private key
    externalip=XXX.XXX.XXX.XXX:7082 ## Replace with your node external IP

**4.2.**  Create file **indexnode.conf** (in 2 folders **~/.index** and **~/.index/testnet3**) contains the following info:
 - LABEL: A one word name you make up to call your node (ex. ZN1)
 - IP:PORT: Your indexnode VPS's IP, and the port is always 18168.
 - INDEXNODEPRIVKEY: This is the result of your "indexnode genkey" from earlier.
 - TRANSACTION HASH: The collateral tx. hash from the 1000 IDX deposit.
 - INDEX: The Index is always 0 or 1.

To get TRANSACTION HASH, run:

    ./src/index-cli indexnode outputs

The output will look like:

    { "d6fd38868bb8f9958e34d5155437d009b72dfd33fc28874c87fd42e51c0f74fdb" : "0", }

Sample of indexnode.conf:

    ZN1 51.52.53.54:18168 XrxSr3fXpX3dZcU7CoiFuFWqeHYw83r28btCFfIHqf6zkMp1PZ4 d6fd38868bb8f9958e34d5155437d009b72dfd33fc28874c87fd42e51c0f74fdb 0

Step 5. Run a indexnode
----------------------
**5.1.**  Start indexnode:

    ./src/index-cli indexnode start-alias <LABEL>

For example:

    ./src/index-cli indexnode start-alias ZN1

**5.2.**  To check node status:

    ./src/index-cli indexnode debug

If not successfully started, just repeat start command
