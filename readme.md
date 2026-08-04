# SDR++Black

![Build Status](https://img.shields.io/github/actions/workflow/status/PhantomRoute/SDRPlusPlusBlack/build_all.yml?branch=master)

Welcome to **SDR++Black**, my personal take on what SDR++ should be!

I originally built this fork for myself because the existing versions didn't have everything I wanted (such as being able to pick which protocols the DSD demodulators sync on, and a cleaner dropdown UI).

Both digital voice modes in the Radio module now have a **Protocols** dropdown. Leave everything ticked and it auto-detects; untick a protocol to stop it syncing on that one.

* **DSD** - P25p1, DMR
* **oldDSD** - P25p1, DMR, NXDN48, NXDN96, D-STAR, X2-TDMA, ProVoice

Your selection is saved per VFO, so NXDN96 (which is off by default) stays on once you enable it.

*This is a fork of SDR++Brown, which itself is a fork of the original SDR++.*

**Please do not report bugs in this fork to the original author.**
If you find issues with SDR++Black, please file an [issue](https://github.com/PhantomRoute/SDRPlusPlusBlack/issues) on this repository.

* Be sure to check out [SDR++Brown](https://github.com/sannysanoff/SDRPlusPlusBrown) for the intermediate fork features.
* Check out the [original upstream project page](https://github.com/AlexandreRouma/SDRPlusPlus) for the original application.

[Changelog](changelog.md)

WINDOWS INSTALL TROUBLESHOOTING: https://youtu.be/Q3CV5U-2IIU

## Thanks / Credits

Thanks and due respect to:
 
* The original author, Alexandre Rouma, for his great [work](https://github.com/AlexandreRouma/SDRPlusPlus). Due credits go to all contributors in the upstream project.
* MSHV author, LZ2HV, for his great [work](http://lz2hv.org/mshv).
* logmmse/python authors for their great [work](https://github.com/wilsonchingg/logmmse).
* OMLSA authors for their great [idea](https://github.com/yuzhouhe2000/OMLSA-IMCRA) and [implementation](https://github.com/xiaochunxin/OMLSA-MCRA).
* imgui-notify author for his great [work](https://github.com/patrickcjk/imgui-notify)
* implot author for his great [work](https://github.com/epezent/implot/)
* alexander-sholohov (github) for his work on soapy_sdr module.
* Cropinghigh / Indir for his [work](github.com/cropinghigh/sdrpp-vhfvoiceradio) on extra VHF modes.
* monolifed for his [pbkdf2 header-only implementation](https://github.com/monolifed/pbkdf2-hmac-sha256)  
* ruse39 for his contributions on code quality

## Feedback

Found an issue? File an [issue](https://github.com/PhantomRoute/SDRPlusPlusBlack/issues).

## Debugging reminders

* to debug in windows in virtualbox env, download mesa opengl32.dll from https://downloads.fdossena.com/Projects/Mesa3D/Builds/MesaForWindows-x64-20.1.8.7z
* make sure you put rtaudiod.dll in the build folder's root otherwise audio sink will not load.
* use system monitor to debug missing dlls while they fail to load.

## Local Android build:

* put into your ~/.gradle/gradle.properties this line: sdrKitRoot=/home/user/SDRPlusPlus/android-sdr-kit/sdr-kit
  * it can obtained + built from: https://github.com/AlexandreRouma/android-sdr-kit 
  * docker build --platform linux/amd64 -t android-sdr-kit  .
  * docker start android-sdr-kit    # it will exit
  * docker cp be03210da56a:/sdr-kit .    # will create directory with built binary libs, replace be03210da56a with id obtained from 'docker ps -a'
* use jdk11 for gradle in android studio. Android Studio -> Settings -> ... -> Gradle -> Gradle JDK . This is needed if you have various errors with java.io unaccessible fields.
* in case of invalid keystore error (should not happen with jdk11): 
  * you may create new keystore with current jdk version:
    ~/soft/jdk8/bin/keytool -genkey -v -keystore debug2.keystore -storepass android -alias androiddebugkey -keypass android -keyalg RSA -keysize 2048 -validity 10000
  * use this filename (debug2.keystore) in app/build.gradle along with passwords in the signingConfigs -> debug section.

Good luck.
