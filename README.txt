otvori ESP-IDF 5.5 PowerShell
i pokreni sa
idf.py build
idf-py -p COM8 flash monitor

ukoliko je program već uploadan 
idf-py -p COM8 monitor

27.08.


program započinje sa wifi ap kako bi se esp prijavio na internet. nakon toga gasi ap i kreće sa primanjem poruka svakih 3000 ms

28-08

update: 
wifi ap se aktivira kako bi se esp prijavio na internet. nakon toga gasi ap i kreće sa primanjem poruka svakih 3000 ms (MOŽE SE MIJENJATI)
nakon gašenja wifi credentials ostaju spremljeni, reset wifi credentialsa za ponovno povezivanje odradi se sa spajanjem GPIO-0 sa GND > 3 sec

