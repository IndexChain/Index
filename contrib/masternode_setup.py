#! /usr/bin/env python
import subprocess
import collections
import os
import sys
import time
import math
import os
import requests,json
from urllib.request import urlopen
from subprocess import *
from crontab import CronTab
SERVER_IP = urlopen('http://ip.42.pl/raw').read()
# BOOTSTRAP_URL = "http://index.org/dprice.zip"
#Change this to match your coin releases
GITHUB_REPO = 'IndexChain/Index'
BINARY_PREFIX = 'index-'
BINARY_SUFFIX='-x86_64-linux-gnu.tar.gz'

DEFAULT_COLOR = "\x1b[0m"
PRIVATE_KEYS = []

def print_info(message):
    BLUE = '\033[36m'
    print(BLUE + "[*] " + str(message) + DEFAULT_COLOR)
    time.sleep(1)

def print_warning(message):
    YELLOW = '\033[93m'
    print(YELLOW + "[*] " + str(message) + DEFAULT_COLOR)
    time.sleep(1)

def print_error(message):
    RED = '\033[91m'
    print(RED + "[*] " + str(message) + DEFAULT_COLOR)
    time.sleep(1)

def get_terminal_size():
    import fcntl, termios, struct
    h, w, hp, wp = struct.unpack('HHHH',
        fcntl.ioctl(0, termios.TIOCGWINSZ,
        struct.pack('HHHH', 0, 0, 0, 0)))
    return w, h
    
def remove_lines(lines):
    CURSOR_UP_ONE = '\x1b[1A'
    ERASE_LINE = '\x1b[2K'
    for l in lines:
        sys.stdout.write(CURSOR_UP_ONE + '\r' + ERASE_LINE)
        sys.stdout.flush()

def run_command(command):
    p = subprocess.run(command,
                         shell=True, check=True)
    return p


def print_welcome():
    os.system('clear')
    print("")
    print("")
    print("")
    print_info("IndexChain masternode installer v1.0")
    print("")
    print("")
    print("")

def update_system():
    print_info("Updating the system...")
    run_command("apt-get update")
    # special install for grub
    run_command('sudo DEBIAN_FRONTEND=noninteractive apt-get -y -o DPkg::options::="--force-confdef" -o DPkg::options::="--force-confold"  install grub-pc')
    run_command("apt-get upgrade -y")

def chech_root():
    print_info("Check root privileges")
    user = os.getuid()
    if user != 0:
        print_error("This program requires root privileges.  Run as root user.")
        sys.exit(-1)

def secure_server():
    print_info("Securing server...")
    run_command("apt-get --assume-yes install ufw")
    run_command("ufw allow OpenSSH")
    run_command("ufw allow 7082")
    run_command("ufw default deny incoming")
    run_command("ufw default allow outgoing")
    run_command("ufw --force enable")

def checkdaemon():
    return os.path.isfile('/usr/local/bin/indexd')

# Helper functions for automating updating and installing daemon
def getlatestrelease():
    r = requests.get(url='https://api.github.com/repos/IndexChain/Index/releases/latest')
    data = json.loads(r.text)['assets']
    for x in range(len(data)):
        if 'x86_64-linux-gnu.tar.gz' in data[x]['browser_download_url']:
            return data[x]['browser_download_url']

def getbinaryname(downloadurl):
    return downloadurl[downloadurl.find(BINARY_prefix):]

def getextfoldername(binaryname):
    return binaryname[:binaryname.find(BINARY_SUFFIX)]

def compile_wallet():
    is_compile = False
    is_download_from_release = True
    if checkdaemon():
        print_warning('Wallet already installed on the system')
        is_download_from_release = False

    if is_download_from_release:
        installdaemon(false)

def installdaemon(fupdate):
    os.system('su - mn1 -c "{}" '.format('index-cli stop &> /dev/null'))
    print_info("Downloading daemon files...")
    binraryurl = getlatestrelease()
    binaryname = getbinaryname(binraryurl)
    foldername = getextfoldername(binaryname)
    run_command("wget " + binraryurl )
    run_command("tar xzf " +binaryname)
    run_command("cd " + foldername +" && cp bin/* /usr/local/bin/ && cd ~")
    if fupdate:
        print_info("Finished updating,now starting mn back up")
        os.system('su - mn1 -c "{}" '.format('indexd -daemon &> /dev/null'))
    else:
        print_info("Finished downloading and installing daemon")

def get_total_memory():
    return (os.sysconf('SC_PAGE_SIZE') * os.sysconf('SC_PHYS_PAGES'))/(1024*1024)

def autostart_masternode(user):
    job = "/usr/local/bin/indexd"
    p = subprocess.Popen("crontab -l -u {} 2> /dev/null".format(user), stderr=STDOUT, stdout=PIPE, shell=True)
    p.wait()
    lines = p.stdout.readlines()
    if job not in lines:
        print_info("Cron job doesn't exist yet, adding it to crontab")
        lines.append(job)
        tab = CronTab(user=user)
        cron = tab.new(command=job)
        cron.every_reboot()
        tab.write()

def setup_first_masternode():
    print_info("Setting up first masternode")
    run_command("useradd --create-home -G sudo mn1")
    print_info("Open your desktop wallet config file (%appdata%/IndexChain/index.conf) and copy your rpc username and password! If it is not there create one! E.g.:\n\trpcuser=[SomeUserName]\n\trpcpassword=[DifficultAndLongPassword]")
    global rpc_username
    global rpc_password
    rpc_username = input("rpcuser: ")
    rpc_password = input("rpcpassword: ")

    print_info("Open your wallet console (Help => Debug window => Console) and create a new masternode private key: indexnode genkey")
    masternode_priv_key = input("indexnodeprivkey: ")
    PRIVATE_KEYS.append(masternode_priv_key)
    
    config = """rpcuser={}
rpcpassword={}
rpcallowip=127.0.0.1
server=1
listen=1
daemon=1
logtimestamps=1
indexnode=1
indexnodeprivkey={}
""".format(rpc_username, rpc_password, masternode_priv_key)

    print_info("Saving config file...")
    #make inital dirs and logs required
    run_command('su - mn1 -c "mkdir /home/mn1/.IndexChain"')
    run_command('su - mn1 -c "touch /home/mn1/.IndexChain/index.conf"')
    run_command('su - mn1 -c "touch /home/mn1/.IndexChain/exodus.log"')
    run_command('su - mn1 -c "touch /home/mn1/.IndexChain/debug.log"')
    f = open('/home/mn1/.IndexChain/index.conf', 'w')
    f.write(config)
    f.close()

    autostart_masternode('mn1')
    os.system('su - mn1 -c "{}" '.format('indexd -daemon &> /dev/null'))
    print_warning("Masternode started syncing in the background...")

def setup_xth_masternode(xth):
    print_info("Setting up {}th masternode".format(xth))
    run_command("useradd --create-home -G sudo mn{}".format(xth))
    run_command("rm -rf /home/mn{}/.IndexChain/".format(xth))

    print_info('Copying wallet data from the first masternode...')
    run_command("cp -rf /home/mn1/.IndexChain /home/mn{}/".format(xth))
    run_command("sudo chown -R mn{}:mn{} /home/mn{}/.IndexChain".format(xth, xth, xth))
    run_command("rm /home/mn{}/.IndexChain/peers.dat &> /dev/null".format(xth))
    run_command("rm /home/mn{}/.IndexChain/wallet.dat &> /dev/null".format(xth))

    print_info("Open your wallet console (Help => Debug window => Console) and create a new masternode private key: indexnode genkey")
    masternode_priv_key = input("indexnodeprivkey: ")
    PRIVATE_KEYS.append(masternode_priv_key)

    BASE_RPC_PORT = 8888
    BASE_PORT = 7082
    
    config = """rpcuser={}
rpcpassword={}
rpcallowip=127.0.0.1
rpcport={}
port={}
server=1
listen=1
daemon=1
logtimestamps=1
indexnode=1
indexnodeprivkey={}
""".format(rpc_username, rpc_password, BASE_RPC_PORT + xth - 1, BASE_PORT + xth - 1, masternode_priv_key)
    
    print_info("Saving config file...")
    f = open('/home/mn{}/.IndexChain/index.conf'.format(xth), 'w')
    f.write(config)
    f.close()
    
    autostart_masternode('mn'+str(xth))
    os.system('su - mn{} -c "{}" '.format(xth, 'indexd  -daemon &> /dev/null'))
    print_warning("Masternode started syncing in the background...")
    

def setup_masternodes():
    memory = get_total_memory()
    setup_first_masternode()

def porologe():

    mn_base_data = """
Alias: zn{}
IP: {}
Private key: {}
Transaction ID: [5k IDX deposit transaction id. 'indexnode outputs']
Transaction index: [5k IDX deposit transaction index. 'indexnode outputs']
mnconf line :
{} {} {} txhash txindex
--------------------------------------------------
"""

    mn_data = ""
    for idx, val in enumerate(PRIVATE_KEYS):
        SERVER_IP_STRING = SERVER_IP + ":".encode('utf-8') + str(7082).encode('utf-8')
        MN_STRING = "zn".encode('ascii') + str(idx+1).encode('ascii')
        mn_data += mn_base_data.format(idx+1, SERVER_IP_STRING.decode(), val, MN_STRING.decode(), SERVER_IP_STRING.decode(), val)

    imp = """"""
    print('')
    print_info(
"""Masternode setup finished!
\tWait until masternode is fully synced. To check the progress login the 
\tmasternode account (su mn1, where 1 is the number of the masternode) and run
\tthe 'index-cli getinfo' to get actual block number. Go to
\t the explorer website to check the latest block number. After the
\tsyncronization is done add your masternodes to your desktop wallet.
Datas:""" + mn_data)

    print_warning(imp)

def main():
    print_welcome()
    chech_root()
    update_system()
    secure_server()
    if checkdaemon():
        installdaemon(True)
    else:
        compile_wallet()
        setup_masternodes()
        porologe()

if __name__ == "__main__":
    main()
