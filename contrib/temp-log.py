#!/usr/bin/env python
# temp-log.py: A script that will be run eventually as a daemon
#              to log temperature of a system
# Usage:
# temp-log.py [-h] [-V] [-l]
# Writes logs to /var/log/ntpstats
# Requires root to run

import argparse
import logging
import logging.handlers
import os
import re
import subprocess
import sys
import time

try:
    import ntp.util
except ImportError as e:
    sys.stderr.write("temp-log: can't find Python NTP modules "
                     "-- check PYTHONPATH.\n")
    sys.stderr.write("%s\n" % e)
    sys.exit(1)


class CpuTemp:
    "Sensors on the CPU Core"
    def __init__(self):
        # pattern that matches the string that has the cpu temp
        self._pattern = re.compile('^\s+temp\d+_input:\s+([\d\.]+).*$')
        # Find the sensors binary and stores the path
        _sensors_path = subprocess.check_output(
            ["which", "sensors"], universal_newlines=False).replace('\n', '')
        if _sensors_path is None:
            raise Exception("Unable to find sensors binary")

    def get_data(self):
        "Collects the data and return the output as an array"
        _index = 0
        _data = []
        # grab the needed output
        _output = subprocess.check_output(['sensors', "-u"],
                                          universal_newlines=True).split('\n')
        for record in _output:
            match = self._pattern.match(record)
            if match and match.group(1):
                _now = int(time.time())
                _cpu_temprature = match.group(1)
                _data.append('%d LM%s %s' % (_now, _index, _cpu_temprature))
                _index += 1

        return _data


class SmartCtl:
    "Sensor on the Hard Drive"
    def __init__(self):
        if os.getuid() != 0:
            raise IOError("You must be root!")
        # Which drive to watch
        self._drives = []
        for child in os.listdir('/dev/'):
            if re.compile('sd[a-z]$').match(child):
                self._drives.append("/dev/"+str(child))
        # this regex matches temperature output lines from smartctl -l
        self._pat = re.compile('194 Temperature_Celsius\s+\S+\s+\d+\s+\d+\s+\d+\s+\S+\s+\S+\s+\S+\s+(\d+)')

    def get_data(self):
        "Collects the data and return the output as an array"
        for _device in self._drives:
            _output = subprocess.check_output(["smartctl", "-a",
                                              _device],
                                              universal_newlines=True
                                              ).split('\n')
            for line in _output:
                match = self._pat.match(line)
                now = int(time.time())
                if match and match.group(1):
                    temp = match.group(1)
                    return ('%d %s %s' % (now, _device, temp))


class ZoneTemp:
    "Zone sensors"
    def __init__(self):
        self._base_dir = '/sys/class/thermal/'
        self.zone_directories = []
        for child in os.listdir(self._base_dir):
            if re.compile('thermal_zone').match(child):
                self.zone_directories.append(child)

    def get_data(self):
        "Collects the data and return the output as an array"
        _zone = 0
        _data = []
        for zone in self.zone_directories:
            _zone_data = open(os.path.join(os.path.join(self._base_dir, zone),
                                           'temp'))
            for line in _zone_data:
                temp = float(line) / 1000
                _now = int(time.time())
                _data.append('%d ZONE%s %s' % (_now, _zone, temp))
                _zone = _zone+1
            _zone_data.close()
        return _data


# Work with argvars
parser = argparse.ArgumentParser(description="Temperature sensor daemon",
                                 epilog="""See the manual page for details.""")
parser.add_argument('-l', '--logfile',
                    dest='logfile',
                    help="append log data to LOGFILE instead of stdout",
                    nargs=1)
parser.add_argument('-o', '--once',
                    dest='once',
                    help="Run the output once and exit",
                    action='store_true')
parser.add_argument('-v', '--verbose',
                    action="store_true",
                    dest='verbose',
                    help="be verbose")
parser.add_argument('-V', '--version',
                    action="version",
                    version="temp-log %s" % ntp.util.stdversion())
parser.add_argument('-w', '--wait',
                    default=[5],
                    dest='wait',
                    help="Set delay time in seconds, default is 5",
                    nargs=1,
                    type=int)
args = parser.parse_args()


def logging_setup(fileName, logLevel):
    "Create logging object"
    logFormat = logging.Formatter('%(message)s')
    # Create logger for cpuTemp
    tempLogger = logging.getLogger()
    tempLogger.setLevel(logLevel)
    # Create file handler
    _file = logging.handlers.TimedRotatingFileHandler(fileName,
                                                      when='midnight',
                                                      interval=1,
                                                      backupCount=5,
                                                      encoding=None,
                                                      delay=False,
                                                      utc=False)
    _file.setLevel(logLevel)
    # Create the formatter and add it to the handler
    _file.setFormatter(logFormat)
    # Add the handler to the logger
    tempLogger.addHandler(_file)
    return tempLogger


def console_log_setup(logLevel):
    "Create logging object that writes to STDOUT"
    logFormat = logging.Formatter('%(message)s')
    # Create the logging object
    tempLog = logging.getLogger()
    tempLog.setLevel(logLevel)
    # Create the output handler
    ch = logging.StreamHandler()
    ch.setLevel(logLevel)
    # Apply the format to the handler
    ch.setFormatter(logFormat)
    # Apply the handler object to the logging object
    tempLog.addHandler(ch)
    return tempLog


def logData(log, data):
    "log the data"
    if type(data) in (tuple, list):
        for _item in data:
            log.info(_item)
    else:
        log.info(data)


def log_data():
    "Write all temperature readings to one file"
    try:
        # Create objects
        cpu = CpuTemp()
        zone = ZoneTemp()
        hdd = SmartCtl()
    except IOError as ioe:
        if args.verbose:
            sys.stderr.write("Unable to run: " + str(ioe) + "\n")
            sys.exit(1)
        else:
            sys.exit(1)
    except Exception as e:
        if args.verbose:
            sys.stderr.write("Unable to run: " + str(e) + "\n")
            sys.exit(1)
        else:
            sys.exit(1)

    # Create the logger instance
    Logger = logging_setup(log, logging.INFO)

    # Create data layout
    logData(Logger, "# Values are space seperated")
    logData(Logger, "# seconds since epoc, sensor, sensor value")

    # Write data to their respective logs forever
    while True:
        logData(Logger, zone.get_data())
        logData(Logger, cpu.get_data())
        logData(Logger, hdd.get_data())
        time.sleep(args.wait[0])


def display():
    try:
        # Create objects
        cpu = CpuTemp()
        zone = ZoneTemp()
        hdd = SmartCtl()
    except IOError as ioe:
        if args.verbose:
            sys.stderr.write("Unable to run: " + str(ioe) + "\n")
            sys.exit(1)
        else:
            sys.exit(1)
    except Exception as e:
        if args.verbose:
            sys.stderr.write("Unable to run: " + str(e) + "\n")
            sys.exit(1)
        else:
            sys.exit(1)

    # Create the logger instance
    Logger = console_log_setup(logging.INFO)
    # Create data layout
    logData(Logger, "# Values are space seperated")
    logData(Logger, "# seconds since epoc, sensor, sensor value")

    # Write data to their respective logs forever
    while True:
        logData(Logger, zone.get_data())
        logData(Logger, cpu.get_data())
        logData(Logger, hdd.get_data())
        time.sleep(args.wait[0])


def one_output():
    "Run the output once"
    try:
        # Create objects
        cpu = CpuTemp()
        zone = ZoneTemp()
        hdd = SmartCtl()
    except IOError as ioe:
        sys.stderr.write("Unable to run: " + str(ioe) + "\n")
        sys.exit(1)
    Logger = console_log_setup(logging.INFO)
    # Create data layout
    logData(Logger, "# Values are space seperated")
    logData(Logger, "# seconds since epoc, sensor, sensor value")
    logData(Logger, zone.get_data())
    logData(Logger, cpu.get_data())
    logData(Logger, hdd.get_data())


args = parser.parse_args()
if args.logfile:
    try:
        log = args.logfile[0]
        log_data()
    except (KeyboardInterrupt, SystemExit):
        sys.exit(0)
elif args.once:
    one_output()
    sys.exit(0)
else:
    try:
        display()
    except (KeyboardInterrupt, SystemExit):
        sys.exit(0)
