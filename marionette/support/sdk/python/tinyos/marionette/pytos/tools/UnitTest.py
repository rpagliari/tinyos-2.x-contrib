#
# Marionette Unit Test Framework
#
# @author Michael Okola
#

import sys, re, os
import pytos.util.NescApp as NescApp

class UnitTest(NescApp.NescApp):
  def __init__( self, buildDir=None, motecom=None, tosbase=True,
                localCommOnly=False, applicationName="Unknown App", xmlFileDOM=None ) :
    if applicationName == "Unknown App":
      raise Exception("Module name must be specified")
    
    #check to see if interface at file applicationName is generic
    interfaceFile = open(applicationName, "r")
    interfaceStr = interfaceFile.read()
    if not interfaceFile:
      raise Exception("Specified file does not exist")
    gen = re.compile('\s*generic\s+configuration\s+', re.MULTILINE)
    result = gen.search(interfaceStr)
    if result != None: #generic
      #interfaceName = interfaceStr[result.end():].split()[0]
      #index = interfaceName.find("(")
      gen = re.compile('(.|\s)*provides(.|\s)+interface\s+')
      end = gen.search(interfaceStr[result.end():]).end() + result.end()
      interfaceName = interfaceStr[end:interfaceStr.find(";", end)]
      #interfaceName = interfaceName[:index]
      generic = True
      
    else:
      gen = re.compile('^\s*configuration\s+', re.MULTILINE)
      result = gen.search(interfaceStr)
      
      if result != None: #not generic
        interfaceName = interfaceStr[result.end():].split()[0]
        generic = False
        
      else: #invalid file
        raise Exception("Configuration information could not be found in file")
      
    #create NesC files
    os.system("rm -rf " + interfaceName + "UnitTest")
    os.mkdir(interfaceName + "UnitTest")
    os.chdir(interfaceName + "UnitTest")
    pFile = open("Test" + interfaceName + "C.nc", "w")
    pFile.write("module Test" + interfaceName + '''P {
  uses {
    interface ''' + interfaceName + ''';
    interface Boot;
    interface SplitControl;
  }
}

implementation {

  event void Boot.booted()
  {
    call SplitControl.start();
  }

  event void SplitControl.startDone(error_t error) {
  }

  event void SplitControl.stopDone(error_t error) {
  }
''')
    pFile.close()
    cFile = open("Test" + interfaceName + "AppC.nc", "w")
    cOutput = "configuration Test" + interfaceName + '''C {
}

implementation {
  
  components Test''' + interfaceName + '''C,
    MainC,
    RamSymbolsM,
    RpcC,
    '''
    if generic:
      cOutput += "new " + interfaceName + "() @rpc();"
    else:
      cOutput += interfaceName + "@rpc();"
    cOutput += '''
      
  Test''' + interfaceName + "C." + interfaceName + " -> " + interfaceName + ''';
  
  Test''' + interfaceName + '''C.Boot -> MainC.Boot;
  Test''' + interfaceName + '''C.SplitControl -> RpcC;
}
'''
    cFile.write(cOutput)
    cFile.close()
      
    #create Makefile
    mFile = open("Makefile", "w")
    mFile.write("COMPONENT=Test" + interfaceName + '''AppC

GOALS += marionette

PFLAGS += -DTOSH_MAX_TASKS_LOG2=8
MARIONETTE_ROOT=/opt/tinyos-2.x-contrib/marionette

''' + #CFLAGS += -I''' + applicationName[:applicationName.rfind("/")] + '''
'''CFLAGS += -I$(MARIONETTE_ROOT)/tos/lib/Rpc
CFLAGS += -I$(MARIONETTE_ROOT)/tos/lib/RamSymbols

TOSMAKE_PATH += $(MARIONETTE_ROOT)/support/make

MAKERULES := $(TOSROOT)/support/make/Makerules
include $(MAKERULES)''')

    mFile.close()
      
    #build and program the mote software
    #os.system("cp " + applicationName + " ./")
    os.system("make " + buildDir + " install")

    #now run the regular NescApp initialization
    #first, import all enums, types, msgs, rpc functions, and ram symbols
    self.applicationName = interfaceName
    self.buildDir = buildDir
    self.motecom=motecom
    self.tosbase=tosbase
    self.localCommOnly=localCommOnly
    try:
      xmlFilename = findBuildFile(buildDir, "nescDecls.xml")
    except:
      raise Exception("""\nERROR: cannot find file \"nescDecls.xml\".

Your nesC app cannot be imported.

Be sure that you compiled with the \"nescDecls\" option.
Be sure that you supplied the correct buildDir parameter \"%s\".

""" % buildDir)
    if xmlFileDOM == None :
      xmlFileDOM = minidom.parse(xmlFilename)

    # Import enums, types, and msgs
    self.enums = NescEnums(xmlFilename, applicationName, xmlFileDOM)
    self.types = NescTypes(xmlFilename, applicationName, xmlFileDOM)
    self.msgs = NescMsgs(self.types, self.enums, applicationName)

    # Connect to the network
    self.connections = []
    if self.motecom != None:
      comm = Comm.Comm()
      comm.connect(self.motecom)
      self.connections.append(comm)

    # Import the rpc commands and ram symbols
    try:
      self.rpc = Rpc.Rpc(self)
    except Exception, e:
      if len(e.args)>0 and re.search("WARNING: cannot find file", e.args[0]) > 0 :
        print  e.args[0]
        return # (if there are no rpc commands, we have nothing left to do)
      else :
        raise
      try:
        self.ramSymbols = RamSymbols.RamSymbols(self, xmlFileDOM=xmlFileDOM)
      except Exception, e:
        if re.search("The RamSymbolsM module was not compiled in", e.args[0]) > 0 :
          print e.args[0]
        else :
          raise
        pass 

    # Create shortcuts to all the application modules
    moduleNames = {}
    self._moduleNames = []
    moduleName = re.compile('^(\w+).')
    names = self.rpc._messages.keys()
    if self.__dict__.has_key("ramSymbols"):
      names.extend(self.ramSymbols._messages.keys())
      names.sort()
      for name in names :
        match = moduleName.match(name)
        if match != None :
          moduleNames[match.groups(0)[0]] = True
          for name in moduleNames.keys() :
            try :
              rpc = self.rpc.__getattr__(name)
            except:
              rpc = None
              try :
                ram = self.ramSymbols.__getattr__(name)
              except:
                ram = None
                self.__dict__[name] = Shortcut(name, rpc, ram)
                self._moduleNames.append(name)
                self._moduleNames.sort()