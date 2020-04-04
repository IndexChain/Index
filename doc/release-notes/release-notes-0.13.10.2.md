# Release v0.13.10.2 : <b>non-mandatory</b> but <b>recommended</b> update 
## Changes in this release:
- Fix connection loss bug on restart of wallet.
- Add filter for Indexnode rewards and stakes.
- Add more inital seeds to sync and connect to.
- Change text in generated/staked block txes accordingly.
- Changed copyright year to 2020.
- Add Waiting for new block to progress text if connected peers are > 1 instead of no block source available.
- Fix mention of zcoin in encryption text.
- Fix getstake to use ismine type from ISMINE_ALL to ISMINE_SPENDABLE (Should fix the fake balance issue some users reported).