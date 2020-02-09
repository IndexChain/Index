#! /usr/bin/env python
import subprocess
import collections
import os
import sys
import time
import math
import os
from urllib.request import urlopen
from subprocess import *
SERVER_IP = urlopen('http://ip.42.pl/raw').read()
# BOOTSTRAP_URL = "http://index.org/dprice.zip"

DEFAULT_COLOR = "\x1b[0m"
PRIVATE_KEYS = []

def print_info(message):
    BLUE = '\033[94m'
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
    print_info("IndexChain masternode(s) installer v1.0")
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

def compile_wallet():
    # print_info("Allocating swap...")
    # run_command("fallocate -l 3G /swapfile")
    # run_command("chmod 600 /swapfile")
    # run_command("mkswap /swapfile")
    # run_command("swapon /swapfile")
    # f = open('/etc/fstab','r+b')
    # line = '/swapfile   none    swap    sw    0   0 \n'
    # lines = f.readlines()
    # if (lines[-1] != line):
    #     f.write(b +line)
    #     f.close()

    print_info("Installing wallet build dependencies...")
    run_command("apt-get --assume-yes install git unzip build-essential libssl-dev libdb++-dev libboost-all-dev libcrypto++-dev libqrencode-dev libminiupnpc-dev libgmp-dev libgmp3-dev autoconf autogen automake libtool")

    is_compile = False
    is_download_from_release = True
    if os.path.isfile('/usr/local/bin/indexd'):
        print_warning('Wallet already installed on the system')
        is_download_from_release = False


    if is_compile:
        print_info("Downloading wallet...")
        run_command("rm -rf /opt/IndexChain")
        run_command("git clone https://github.com/IndexChain/Index /opt/IndexChain")
        
        print_info("Compiling wallet...")
        run_command("chmod +x /opt/IndexChain/src/leveldb/build_detect_platform")
        run_command("chmod +x /opt/IndexChain/src/secp256k1/autogen.sh")
        run_command("cd  /opt/IndexChain/src/ && make -f makefile.unix USE_UPNP=-")
        run_command("strip /opt/IndexChain/src/indexd")
        run_command("cp /opt/IndexChain/src/indexd /usr/local/bin")
        run_command("cd /opt/IndexChain/src/ &&  make -f makefile.unix clean")
        run_command("indexd")
    if is_download_from_release:
        print_info("Downloading daemon files...")
        run_command("wget https://github.com/IndexChain/Index/releases/download/v0.13.8.11/index-0.13.8-x86_64-linux-gnu.tar.gz")
        #Assuming the command went well,extract the targz
        run_command("tar xzf index-0.13.8-x86_64-linux-gnu.tar.gz")
        run_command("cd index-0.13.8 && cp bin/* /usr/local/bin/ && cd ~")
        print_info("Finished downloading and installing daemon/wallet")


def get_total_memory():
    return (os.sysconf('SC_PAGE_SIZE') * os.sysconf('SC_PHYS_PAGES'))/(1024*1024)

def autostart_masternode(user):
    job = b"@reboot /usr/local/bin/indexd\n"
    
    p = subprocess.Popen("crontab -l -u {} 2> /dev/null".format(user), stderr=STDOUT, stdout=PIPE, shell=True)
    p.wait()
    lines = p.stdout.readlines()
    if job not in lines:
        print_info("Cron job doesn't exist yet, adding it to crontab")
        lines.append(job)
        p = subprocess.run("echo \"{}\" | crontab -u {} -".format(''.join(lines).decode("utf-8"), user).decode("utf-8"), shell=True)
        p.wait()

def setup_first_masternode():
    print_info("Setting up first masternode")
    run_command("useradd --create-home -G sudo mn1")
    os.system('su - mn1 -c "{}" '.format("indexd -daemon &> /dev/null"))

    print_info("Open your desktop wallet config file (%appdata%/IndexChain/index.conf) and copy your rpc username and password! If it is not there create one! E.g.:\n\trpcuser=[SomeUserName]\n\trpcpassword=[DifficultAndLongPassword]")
    global rpc_username
    global rpc_password
    rpc_username = input("rpcuser: ")
    rpc_password = input("rpcpassword: ")

    print_info("Open your wallet console (Help => Debug window => Console) and create a new masternode private key: znode genkey")
    masternode_priv_key = input("znodeprivkey: ")
    PRIVATE_KEYS.append(masternode_priv_key)
    
    config = """rpcuser={}
rpcpassword={}
rpcallowip=127.0.0.1
server=1
listen=1
daemon=1
logtimestamps=1
znode=1
znodeprivkey={}
""".format(rpc_username, rpc_password, masternode_priv_key)

    print_info("Saving config file...")
    f = open('/home/mn1/.IndexChain/index.conf', 'w')
    f.write(config)
    f.close()

    # print_info("Downloading blockchain bootstrap file...")
    # run_command('su - mn1 -c "{}" '.format("cd && wget --continue " + BOOTSTRAP_URL))
    
    # print_info("Unzipping the file...")
    # filename = BOOTSTRAP_URL[BOOTSTRAP_URL.rfind('/')+1:]
    # run_command('su - mn1 -c "{}" '.format("cd && unzip -d .IndexChain -o " + filename))

    # run_command('rm /home/mn1/.IndexChain/peers.dat') 
    autostart_masternode('mn1')
    run_command("runuser -l  mn1 -c 'killall indexd'")
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

    print_info("Open your wallet console (Help => Debug window => Console) and create a new masternode private key: znode genkey")
    masternode_priv_key = input("znodeprivkey: ")
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
znode=1
znodeprivkey={}
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
    masternodes = int(math.floor(memory / 300))
    print_info("This system is capable to run around {} masternodes. To support IndexChain network only use one masternode per ip.".format(masternodes))
    print_info("How much masternodes do you want to setup?")
    masternodes = int(input("Number of masternodes: "))
   
    if masternodes >= 1:
        setup_first_masternode()

    for i in range(masternodes-1):
        setup_xth_masternode(i+2)

def porologe():

    mn_base_data = """
Alias: Masternode{}
IP: {}
Private key: {}
Transaction ID: [5k deposit transaction id. 'znode outputs']
Transaction index: [5k deposit transaction index. 'znode outputs']
--------------------------------------------------
"""

    mn_data = ""
    for idx, val in enumerate(PRIVATE_KEYS):
        mn_data += mn_base_data.format(idx+1, SERVER_IP +str.encode( ":" + str(7082 + idx)), val)

    imp = """"""
    print('')
    print_info(
"""Masternodes setup finished!
\tWait until all masternodes are fully synced. To check the progress login the 
\tmasternode account (su mnX, where X is the number of the masternode) and run
\tthe 'indexd getinfo' to get actual block number. Go to
\t the explorer website to check the latest block number. After the
\tsyncronization is done add your masternodes to your desktop wallet.
Datas:""" + mn_data)

    print_warning(imp)

def main():
    print_welcome()
    chech_root()
    update_system()
    secure_server()
    compile_wallet()
    setup_masternodes()
    porologe()

if __name__ == "__main__":
    main()
