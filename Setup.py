import os
import platform
import shutil
import subprocess
import sys

if __name__ == '__main__':
    print('Your detected System is: ' + platform.system())
    
    # If the user runs the script for the first time, make sure all dependencies exist
    if (not os.path.exists('bin')):
        # The large files in git lfs are the Windows only OpenCV binaries, Linux uses the OpenCV of the system.
        if (platform.system() != 'Linux'):
            print('Running the installer for the first time, making sure all lfs data is up-to-date...')
            subprocess.call(["git", "lfs", "pull"])
        subprocess.call(["git", "submodule", "update", "--init", "--recursive"])

    print('Running premake...')

    if (platform.system() == 'Windows'):
        subprocess.call(["vendor/bin/premake/Windows/premake5.exe", "vs2022"])
    elif (platform.system() == 'Linux'):
        # Prefer a premake5 installed on the system. The bundled one only runs on x86_64, not on ARM (e.g. a Raspberry Pi).
        premake = shutil.which('premake5')
        if premake is None and platform.machine() in ('x86_64', 'AMD64'):
            premake = 'vendor/bin/premake/Linux/premake5'
            subprocess.call(["chmod", "+x", premake])

        if premake is None:
            print('No premake5 found for ' + platform.machine() + '. Install premake5 (see linux_dependencies.md) and run this script again.')
            sys.exit(1)

        subprocess.call([premake, "gmake"])
    elif (platform.system() == 'Darwin'):
        subprocess.call(["chmod", "+x", "vendor/bin/premake/MacOS/premake5"])
        subprocess.call(["vendor/bin/premake/MacOS/premake5", "xcode4"])
        
