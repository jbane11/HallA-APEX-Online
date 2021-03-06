//////////////////////////////////////////////////////////////////
//
//   THaScaler
//
//   Scaler Data in Hall A at Jlab.  
//   The usage covers several implementations:
//
//   1. Works within context of full analyzer, or standalone.
//   2. Time dependent channel mapping to account for movement
//      of channels or addition of new channels (scaler.map).
//   3. Source of data is either THaEvData, CODA file, 
//      online (VME), or scaler-history-file.
//   4. Optional displays of rates, counts, history.
//
//   Terminology:
//      bankgroup = group of scaler banks, e.g. Left Spectrometer crate.
//      bank  = group of scalers, e.g. S1L PMTs.
//      slot  = slot of scaler channels (1 module in VME)
//      channel = individual channel of scaler data
//      normalization scaler =  bank assoc. with normalization (charge, etc)
//
//   scaler.map file determines the layout.  See THaScalerDB class.
//   *TO ADD A NEW SCALER* bank you need to add lines to DataMap in
//   THaScaler::InitData 
//
//   author  Robert Michaels (rom@jlab.org)
//
/////////////////////////////////////////////////////////////////////

#include "THaScaler.h"
#include "THaScalerDB.h"
#include "THaCodaFile.h"
#include "THaEvData.h"
#include "TDatime.h"
#include <sys/ioctl.h>
#include <sys/types.h>
#include <cstdio>
#include <unistd.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string>

#include <iostream>

using namespace std;

THaScaler::THaScaler( const char* bankgr ) {
// Set up the scaler banks.  Each bank is a group of related scalers.
// 'bankgr' is group of scaler banks, "Left"(L-arm), "Right"(R-arm), etc
  database = 0;
  if( !bankgr || !*bankgr ) {
    MakeZombie();
    return;
  }
  bankgroup = bankgr;
  did_init = kFALSE;
  coda_open = kFALSE;
  new_load = kFALSE;
  one_load = kFALSE;
  use_clock = kTRUE;
  vme_server = "";
  vme_port = 0;
  normslot = new Int_t[3];
  memset(normslot, -1, 3*sizeof(Int_t));
  clkslot = -1;
  clkchan = -1;
  fcodafile = new THaCodaFile;
  rawdata = new Int_t[2*SCAL_NUMBANK*SCAL_NUMCHAN];
  memset(rawdata,0,2*SCAL_NUMBANK*SCAL_NUMCHAN*sizeof(Int_t));
  clockrate = 1024;  // a default 
};

THaScaler::~THaScaler() {
   if (database) delete database;
   if (rawdata) delete [] rawdata;
   if (fcodafile) delete fcodafile;
   if (normslot) delete [] normslot;
};

Int_t THaScaler::Init( const TDatime& time ) 
{
  // Initialize scalers for given date/time.
  // Accuracy is 1 day. Only date is used, time is ignored.

  Int_t i = time.GetDate();
  char date[11];
  sprintf( date, "%2.2d-%2.2d-%4.4d",i%100,(i%10000-i%100)/100,i/10000 );
  return Init( date );
};

Int_t THaScaler::Init(const char* thetime ) 
{
  // Init() is required to be run once in the life of object.
  // 'thetime' is the Day-Month-Year to look up scaler map related to data.
  // e.g. thetime = '21-05-1999' --> 21st of May 1999.
  // 'thetime' can also be "now"

  string strtime(thetime);

  int year, month, day;
  if (strtime == "now") {
    time_t t1 = time(0);
    struct tm* btm = gmtime(&t1);
    year =  1900 + btm->tm_year;  
    month = 1 + btm->tm_mon;  // Jan,Feb,Mar = 1,2,3...
    day = btm->tm_mday;
  } else {
    int pos1 = strtime.find_first_of("-");
    int pos2 = strtime.find_last_of("-");
    if (pos1 > 0 && pos2 > pos1) {
      string sday= strtime.substr(0,pos1);
      string smonth = strtime.substr(pos1+1,pos2-pos1-1);
      string syear = strtime.substr(pos2+1,strtime.size()-pos2);
      day = atoi(sday.c_str());
      month = atoi(smonth.c_str());
      year = atoi(syear.c_str());
    } else {
      return SCAL_ERROR;
    }
  }

  Bdate date_want(day,month,year);    // date when we want data.

  database = new THaScalerDB();
  int status = InitData(bankgroup, date_want);
  if (status == SCAL_ERROR) return SCAL_ERROR;

  if ( database->extract_db(date_want) == kFALSE) {
    // This is not necessarily fatal, but is usually bad...
    cout << "THaScaler:: WARNING:  Failed to extract scaler database"<<endl;
    delete database;
    database = 0;
    return -1; 
  }

  SetupNormMap();

  did_init = kTRUE;
    
  return 0;
};

Int_t THaScaler::InitData(std::string bankgroup, const Bdate& date_want) {
// Initialize data of the class for this bankgroup for the date wanted.
// While in principle this should come from a "database", it basically
// never changes.  However, to add a new scaler bank, imitate what is
// done for "Left" or "Right" HRS.
// Note: this routine must be called after database is new'd but 
// before it is loaded.

  crate = -1;

  struct DataMap {
    const char *bank_name;      // name of bank ("Left", "Right", "dvcs", etc)
    Int_t bank_header;          // header to find data
    Int_t bank_cratenum;        // crate number
    Int_t evstr_type;           // part of event stream (1) or evtype 140
    Int_t normslot;             // slot of normalization data (database can
                                // overwrite this)
    Double_t bank_clockrate;    // clock rate
    std::string bank_IP;        // IP address of server for online data
    Int_t bank_port;            // port number of server
    Int_t bank_onlmap[SCAL_NUMBANK]; // map of channels for online server
  };

  static const DataMap datamap[] = {
    // Event type 140's
    { "Left",  0xabc00000, 8, 140, 4, 1024, "129.57.192.30",  5022, 
             0,1,2,3,4,5,6,7,8,9,10,11 },
    { "Right", 0xceb00000, 7, 140, 8, 1024, "129.57.192.28",  5021, 
             0,1,2,3,4,5,6,7,8,9,10,11 },
    { "dvcs",  0xd0c00000, 9, 140, 0, 105000, "129.57.192.51",  5064, 
             0,1,2,3,4,5,6,7,8,9,10,11 },
    // N20: header 0xbba...,  crate=6 (our choice), 
    { "N20",  0xbba00000, 6, 140, 1, 2048, "129.57.192.51",  5064, 
             0,1,2,3,4,5,6,7,8,9,10,11 },
    // Data that are part of the event stream
    { "evleft",  0xabc00000, 11, 1, 4, 1024, "none",  0, 0,0,0,0,0,0,0,0,0,0,0,0},
    { "evright", 0xceb00000, 10, 1, 8, 1024, "none",  0, 0,0,0,0,0,0,0,0,0,0,0,0},
   // Add new scaler bank here...
    { 0 }

  };


   std::string bank_to_find = "unknown";
   if (database) {
     if ( database->FindNoCase(bankgroup,"Left") != std::string::npos  
        || bankgroup == "L" ) {
          bank_to_find = "Left"; 
     }
     if ( database->FindNoCase(bankgroup,"Right") != std::string::npos  
        || bankgroup == "R" ) {
          bank_to_find = "Right"; 
     }
     if ( database->FindNoCase(bankgroup,"dvcs") != std::string::npos) {
          bank_to_find = "dvcs"; 
     }
     if ( database->FindNoCase(bankgroup,"N20") != std::string::npos) {
          bank_to_find = "N20"; 
     }
     if ( database->FindNoCase(bankgroup,"evleft") != std::string::npos) {
          bank_to_find = "evleft"; 
     }
     if ( database->FindNoCase(bankgroup,"evright") != std::string::npos) {
          bank_to_find = "evright"; 
     }

   }

// Handle the detector swap for data prior to Sept 15, 2000
  Bdate dswap(15,9,2000);     
  if (date_want < dswap) {
    string stemp = bank_to_find;
    if (stemp == "Left") bank_to_find = "Right";  // swap
    if (stemp == "Right") bank_to_find = "Left";  // L <-> R
  }

  onlmap.clear();

  if( const DataMap* it = datamap ) {
    while( it->bank_name ) {
      string sbank = it->bank_name;
      if( bank_to_find == sbank ) {  
        header = it->bank_header;
        crate = it->bank_cratenum;
        evstr_type = it->evstr_type;
        normslot[0] = it->normslot;
        if (use_clock) clockrate = it->bank_clockrate;
        vme_server = it->bank_IP;
        vme_port = it->bank_port;
        for (int i = 0; i < SCAL_NUMBANK; i++) onlmap.push_back(it->bank_onlmap[i]);
      }
      if (database) {
          database->LoadCrateToInt(it->bank_name, it->bank_cratenum);
          if (fDebug) {
            cout << "crate corresp. "<<it->bank_name;
            cout << " = "<<database->CrateToInt(string(it->bank_name))<<endl;
	  }
      }
      it++;
    }
  }
  normslot[1] = normslot[0]-1;
  normslot[2] = normslot[0]+1;

  if (fDebug) {
    cout << "Set up bank "<<bank_to_find<<endl;
    cout << "crate "<<crate<<"   header 0x"<<hex<<header<<dec<<endl;
    cout << "default normalization slot "<<normslot<<endl;
    cout << "evstr_type "<<evstr_type<<"  clock rate "<<clockrate<<endl;
    cout << "vme: "<<vme_server<<"  "<<vme_port<<endl;
    cout << "online map: "<<endl;
    for (int i = 0; i < SCAL_NUMBANK; i++) cout << " "<<onlmap[i];
    cout << endl;
   }

// Calibration of BCMs
  calib_u1 = 1345;  calib_u3 = 4114;  calib_u10 = 12515; 
  calib_d1 = 1303;  calib_d3 = 4034;  calib_d10 = 12728; 
  off_u1 = 92.07;   off_u3 = 167.06;  off_u10 = 102.62;  
  off_d1 = 72.19;   off_d3 = 81.08;   off_d10 = 199.51;

  if (crate == -1) {
    if (SCAL_VERBOSE) {
      cout << "THaScaler:: Warning: Undefined crate"<<endl;
      cout << "Need to Init for 'Left', 'Right' crate, etc."<<endl;
    }
  }

  return 0;
};

void THaScaler::SetIpAddress(std::string ipaddress) 
{ // Set IP address used by online code 
  vme_server = ipaddress;
}

void THaScaler::SetPort(Int_t port) 
{ // Set port# used by online code 
  vme_port = port;
}
   
void THaScaler::SetClockLoc(Int_t slot, Int_t chan)
{ // Setup the clock.  If this is called, it over-rides the
  // definition of "clock" in the scaler.map file.  If this is
  // not called, then the "clock" is defined by scaler.map
  if (slot == -1) {  // assume slot is the normalization scaler 
    clkslot = GetSlot("TS-accept");
  } else {
    clkslot = slot;
  }
  clkchan = chan;
}

void THaScaler::SetupNormMap() {
  if (!database) return;
// Retrieve info about the normalization scaler, which
// is the one that contains TS-accept.  This makes
// subsequent code much faster.
  normslot[0] = GetSlot("TS-accept");
  normslot[1] = GetSlot("TS-accept", -1);
  normslot[2] = GetSlot("TS-accept",  1);
  clkslot = GetSlot("clock");
  clkchan = GetChan("clock");
  for (Int_t ichan = 0; ichan < SCAL_NUMCHAN; ichan++) {
    std::vector<std::string> chan_name = database->GetShortNames(crate, normslot[0], ichan);
    for (int i = 0; i < chan_name.size(); i++) {
      if (chan_name[i] != "none") {
        normmap.insert(make_pair(chan_name[i], ichan));
      }
    }
  }
}

void THaScaler::SetClockRate(Double_t rate)
{ 
  clockrate = rate;
}

void THaScaler::SetTimeInterval(Double_t time)
{ 
// Set the average time interval between events.
// Note, use this *ONLY IF* there is no clock in the datastream.
  if (time <= 0) {
    cout << "THaScaler::SetTimeInterval:ERROR:  nonsensical time value"<<endl;
    return;
  } else {
    clockrate = 1/time;
    use_clock = kFALSE;
  }
}

Int_t THaScaler::LoadData(const THaEvData& evdata) {
// Load data from THaEvData object.  Return of 0 is ok.
// Note: GetEvBuffer is no faster than evdata.Get...
  static int ldebug = 0;
  static Int_t data[2*SCAL_NUMBANK*SCAL_NUMCHAN];
  new_load = kFALSE;
  Int_t nlen = 0;
  if (evstr_type == 1) {  // data in the event stream (physics triggers)
    if (!evdata.IsPhysicsTrigger()) return 0;  
    nlen = evdata.GetRocLength(crate); 
  } else {  // traditional scaler event type 140
    if (evdata.GetEvType() != 140) return 0;   
    nlen = evdata.GetEvLength();
  }
  if (ldebug) cout << "Loading evdata, bank =  "<<bankgroup<<"  "<<evstr_type<<"  "<<crate<<"  "<<evdata.GetEvType()<<"   "<<nlen<<endl;
  for (Int_t i = 0; i < nlen; i++) {
    if (evstr_type == 1) {
      data[i] = evdata.GetRawData(crate, i);
    } else {
      data[i] = evdata.GetRawData(i);
    }
    if (ldebug) {
      cout << "  data["<<i<<"] = "<<data[i]<<"  =0x"<<hex<<data[i]<<dec<<endl;
    } 
  }
  ExtractRaw(data,nlen);  
  return 0;
};

Int_t THaScaler::LoadDataCodaFile(const char* filename) { 
// from CODA file 'filename'.  We'll open it for you. Return of 0 is ok.
  TString file(filename);
  return LoadDataCodaFile(file);
};

Int_t THaScaler::LoadDataCodaFile(TString filename) { 
// from CODA file 'filename'.  We'll open it for you. Return of 0 is ok.
  new_load = kFALSE;
  if ( !coda_open ) {
     fcodafile->codaOpen(filename);
     coda_open = kTRUE;
  } 
  return LoadDataCodaFile(fcodafile);
};

Int_t THaScaler::LoadDataCodaFile(THaCodaFile *codafile) {
// Load data from a CODA file, assumed to be already opened. 
// Return of 0 means end of data, return of 1 means there is more data.
  new_load = kFALSE;
  if (CheckInit() == SCAL_ERROR) return SCAL_ERROR;
  int codastat, extstat;
  found_crate = kFALSE;
  codastat = 0;
  while (codastat == 0) {
    codastat = codafile->codaRead();  
    if (codastat < 0) return 0;
    const Int_t *data = codafile->getEvBuffer();
    int evtype = data[1]>>16;
    if (evtype == SCAL_EVTYPE) {
      extstat = ExtractRaw(data);
      if (extstat) return 1;
    }
  }
  return 0;
};

Int_t THaScaler::ExtractRaw(const Int_t* data, int dlen) {
// Extract rawdata from data if this event belongs to this scaler crate.
// This works for CODA event data or VME data format but not 
// for scaler history file (strings).
  int len, ndat, max, i, j, k, slot, numchan;
  found_crate = kFALSE;
  Int_t lfirst = 1;
  len = dlen;
  if (!data) return 0;
  if (dlen == 0) len  = data[0] + 1;
  max = fcodafile->getBuffSize();
  ndat = len < max ? len : max;
// Sanity check:  If ndat > 10000 this is crazy (normally ~300).
  if (ndat > 10000) {
     cout << "THaScaler:: WARNING:  The event length is crazy."<<endl;
     cout << "Skipping corrupted scaler event."<<endl;
     Clear();
     return 0;
  }
  for (i = 0; i < ndat; i++) {
    if (((data[i]&0xfff00000) == (unsigned long)header) &&
    ((data[i]&0x0000ff00) == 0) ) { // found this crate's data
      if (lfirst) {
        lfirst = 0;
        LoadPrevious();
        Clear();
      }
      slot = (data[i]&0xf0000)>>16;
      numchan = (data[i]&0xff);
      if (numchan == 0) numchan = 32;  // happens for event stream
      for (j = i+1; j < i+numchan+1; j++) {
         k = slot*SCAL_NUMCHAN + j-i-1;
         if (k >= 0 && k < SCAL_NUMBANK*SCAL_NUMCHAN) rawdata[k] = data[j];
      }
      found_crate = kTRUE;
    }  
  }
  if (found_crate) {
     new_load = kTRUE;
     one_load = kTRUE;
     return 1;
  }
  return 0;
};  

Int_t THaScaler::LoadDataHistoryFile(int run_num) {
// Load data from scaler history file for run number
  return LoadDataHistoryFile("scaler_history.dat", run_num);
};

Int_t THaScaler::LoadDataHistoryFile(const char* filename, int run_num) {
// Load data from scaler history file 'filename' for run number run_num
  new_load = kFALSE;
  if (CheckInit() == SCAL_ERROR) return SCAL_ERROR;
  ClearAll();
  ifstream hfile(filename);
  if ( !hfile ) {
    cout << "ERROR: THaScaler:  Scaler history file "<<filename;
    cout << " does not exist."<<endl<<"Hence, no data."<<endl;
    return SCAL_ERROR;
  }
  char *crun_num; crun_num = new char[20]; 
  sprintf(crun_num," %d",run_num);
  string runstr("run number");
  string myrun = runstr + crun_num;
  string sinput,dat;
  int foundrun = -1;
  while (getline(hfile, sinput)) {
    foundrun = sinput.find(myrun,0);
    if (foundrun >= 0) {
      while (getline(hfile, dat)) {
        int nextrun = dat.find(runstr,0);
        if (nextrun >= 0) goto decoder;
	UInt_t htst = header_str_to_base16(dat);
        if ((htst&0xfff00000) == (unsigned long)header) {
          int slot = (htst&0xf0000)>>16;
	  int numchan = (htst&0xff);
          for (int j = 0; j < numchan; j++) {
            getline(hfile, dat);
	    int k = slot*SCAL_NUMCHAN + j;
            if (k >= 0 && k < SCAL_NUMBANK*SCAL_NUMCHAN) {
 	      rawdata[k] = atoi(dat.c_str());
	    }
	  }
	}
      }
    }
  }
  if (foundrun < 0 && SCAL_VERBOSE == 1) {
    cout << "WARNING: THaScaler: Did not find run "<<run_num<<endl;
    cout << "in scaler history file"<<endl<<"Hence, no data."<<endl;
    return SCAL_ERROR;
  }
decoder:
  new_load = kTRUE;
  one_load = kTRUE;
  delete [] crun_num;
  return 0;
};

Int_t THaScaler::LoadDataOnline() {
// Load data from online VME server and port for this 'Bank Group'
  return LoadDataOnline(vme_server.c_str(), vme_port);
};

Int_t THaScaler::LoadDataOnline(const char* server, int port) {
// Load data from VME 'server' and 'port'.

// Structure for requests from this client to VME server 
#define MAXBLK   20
#define MSGSIZE  50
struct request { 
   int reply;
  int ibuf[16*MAXBLK];  /* need int instead of long, to match to 32bit server*/
   char message[MSGSIZE];
   int clearflag; int checkend;
};
#define SERVER_WORK_PRIORITY 100
#define SERVER_STACK_SIZE 10000
#define MAX_QUEUED_CONNECTIONS 5
#define SOCK_ADDR_SIZE  sizeof(struct sockaddr_in)
#define REPLY_MSG_SIZE 1000

  new_load = kFALSE;
  if (CheckInit() == SCAL_ERROR) return SCAL_ERROR;

  int   sFd;      //  socket file descriptor 
  int i, k, slot, nchan, ntot, sca;
  unsigned long nRead, nRead1;
  struct request myRequest;           //  request to send to server 
  struct request vmeReply;            //  reply from server
  struct sockaddr_in serverSockAddr;  //  socket addr of server
  static int lprint   = 0;
  myRequest.clearflag = 0;
  myRequest.checkend  = 0;
  int timeoutcnt=0;

// create socket 
  if ((sFd = socket (PF_INET, SOCK_STREAM, 0)) == -1 ) {
      cout << "ERROR: THaScaler: LoadDataOnline: Cannot open socket"<<endl;
      return SCAL_ERROR;
  }
  serverSockAddr.sin_family = PF_INET;
  serverSockAddr.sin_port = htons(port);
  serverSockAddr.sin_addr.s_addr = inet_addr(server);

// connect to server 
  if(connect (sFd, (struct sockaddr *) &serverSockAddr, sizeof(serverSockAddr)) == -1) {
      cout << "ERROR: THaScaler: LoadDataOnline: Cannot connect "<<endl;
      cout << "to VME server"<<endl;
      return SCAL_ERROR;
  }

// send request to server, read reply
  myRequest.reply = 1;
  if(write(sFd, (char *)&myRequest, sizeof(myRequest)) == -1) {
       cout << "ERROR: THaScaler: LoadDataOnline: Cannot write "<<endl;
       cout << "request to VME server"<<endl;
       return SCAL_ERROR;
    }
    nRead = read (sFd, (char *)&vmeReply, sizeof(vmeReply));
    if(nRead < 0) {
       printf("ERROR: THaScaler: reading from scaler server\n");
       exit(0);
    }
    while (nRead < sizeof(vmeReply)) {
       nRead1 = read (sFd, ((char *) &vmeReply)+nRead,
                    sizeof(vmeReply)-nRead);
       if (timeoutcnt++ > 50) break;
       if(nRead1 < 0) {
	 cout << "Error from reading from scaler server\n"<<endl;
         return SCAL_ERROR;
       }
       nRead += nRead1;
  }

  close (sFd);

  LoadPrevious();
  Clear();

  for (k = 0 ; k < 16*MAXBLK; k++) {
       vmeReply.ibuf[k] = ntohl(vmeReply.ibuf[k]);
  }
  ntot = 0;
  for (slot = 0; slot < SCAL_NUMBANK; slot++) {
    int jslot = onlmap[slot];
    if (slot >= MSGSIZE) {
      cout << "ERROR: THaScaler: LoadDataOnline:"<<endl;
      cout << "Cannot parse slot "<<slot<<endl;
      return SCAL_ERROR;
    }
    nchan = 0;
    if (vmeReply.message[slot]=='0') nchan = 16;
    if (vmeReply.message[slot]=='1') nchan = 32;
    if (nchan == 0) goto onldone;
    for (k = 0; k < nchan; k++) {
      i = jslot*SCAL_NUMCHAN + k;
      if (i < SCAL_NUMBANK*SCAL_NUMCHAN && ntot < 16*MAXBLK) {
	 // note, it was already "ntohl" above
         rawdata[i] = vmeReply.ibuf[ntot++];
      } else {
         if (SCAL_VERBOSE) {
           cout << "WARNING: THaScaler: LoadDataOnline:"<<endl;
           cout << "Truncation of data or improper array index"<<endl;
         }
      }
    }
  }
onldone:
  new_load = kTRUE;
  one_load = kTRUE;
  if (lprint == 1) {  // Debug printout
     printf ("\n\n\n  ========    Scalers   ===============\n\n");
     sca = 0;
     for (k = 0; k < ntot/16; k++) {
       sca = 2*k;
       for (i = 0; i < 2; i++) {
         printf ("%3d :  %8d %8d %8d %8d %8d %8d %8d %8d \n",8*(sca+i)+1,
        (int)vmeReply.ibuf[(sca+i)*8],(int)vmeReply.ibuf[(sca+i)*8+1],
        (int)vmeReply.ibuf[(sca+i)*8+2],(int)vmeReply.ibuf[(sca+i)*8+3],
        (int)vmeReply.ibuf[(sca+i)*8+4],(int)vmeReply.ibuf[(sca+i)*8+5],
        (int)vmeReply.ibuf[(sca+i)*8+6],(int)vmeReply.ibuf[(sca+i)*8+7]);
       }
     }
  }
  return 0;
};
   
void THaScaler::Print(Option_t* opt) const {
// Print data contents
  int i,j;
  cout << "\n============== Print out ================"<<endl;
  cout << "THaScaler Data for bankgroup = "<<bankgroup<<endl;
  cout << "Header "<<hex<<header<<"  crate num "<<dec<<crate<<endl;   
  cout << "Raw data = "<<endl;
  for (i = 0; i < SCAL_NUMBANK; i++) {
    for (j = 0; j < 8; j++) cout << hex << rawdata[i*32+j] << " ";
    cout << endl;
    for (j = 0; j < 8; j++) cout << hex << rawdata[i*32+j+8] << " ";
    cout << endl;
    for (j = 0; j < 8; j++) cout << hex << rawdata[i*32+j+16] << " ";
    cout << endl;
    for (j = 0; j < 8; j++) cout << hex << rawdata[i*32+j+24] << " ";
    cout << "\n-------"<< dec << endl;
  }
};

void THaScaler::PrintSummary() {
// Print out a summary of important scalers 
  if (CheckInit() == SCAL_ERROR) {
    cout << "THaScaler: WARNING:  You never initialized scalers."<<endl;
    cout << "Must call Init method once in the life of object."<<endl;
  }
  printf("\n -------------   Scaler Summary   ---------------- \n");
  cout << "Scaler bank  " << bankgroup << endl;
  Double_t time_sec = GetPulser("clock")/clockrate;
  if (time_sec == 0) {
    cout << "THaScaler: WARNING:  Time of run = ZERO (\?\?)\n"<<endl;
    return;
  } 
  Double_t time_min = time_sec/60;  
  Double_t curr_u1  = ((Double_t)GetBcm("bcm_u1")/time_sec - off_u1)/calib_u1;
  Double_t curr_u3  = ((Double_t)GetBcm("bcm_u3")/time_sec - off_u3)/calib_u3;
  Double_t curr_u10 = ((Double_t)GetBcm("bcm_u10")/time_sec - off_u10)/calib_u10;
  Double_t curr_d1  = ((Double_t)GetBcm("bcm_d1")/time_sec - off_d1)/calib_d1;
  Double_t curr_d3  = ((Double_t)GetBcm("bcm_d3")/time_sec - off_d3)/calib_d3;
  Double_t curr_d10 = ((Double_t)GetBcm("bcm_d10")/time_sec - off_d10)/calib_d10;
  printf("Time of run  %7.2f min \n",time_min);
  printf("Triggers:     1 = %d    2 = %d    3 = %d   4 = %d    5 = %d\n",
         GetTrig(1),GetTrig(2),GetTrig(3),GetTrig(4),GetTrig(5));
  printf("Accepted triggers:   %d \n",GetNormData(0,"TS-accept"));
  printf("Accepted triggers by helicity:    (-) = %d    (+) = %d\n",
         GetNormData(-1,"TS-accept"),GetNormData(1,"TS-accept"));
  printf("Charge Monitors  (Micro Coulombs)\n");
  printf("Upstream BCM   gain x1 %8.2f     x3 %8.2f     x10 %8.2f\n",
     curr_u1*time_sec,curr_u3*time_sec,curr_u10*time_sec);
  printf("Downstream BCM   gain x1 %8.2f     x3 %8.2f     x10 %8.2f\n",
     curr_d1*time_sec,curr_d3*time_sec,curr_d10*time_sec);
};

Int_t THaScaler::GetScaler(Int_t slot, Int_t chan, Int_t histor) {
// Get data by slot and channel.  This is the FASTEST method.
// Histor = 0 = present event.  Histor = 1 = previous event
  Int_t index;
  index = SCAL_NUMCHAN*slot + chan;
  if (histor == 1) index += SCAL_NUMBANK*SCAL_NUMCHAN;
  if (index >=0 && index < 2*SCAL_NUMBANK*SCAL_NUMCHAN) {
    return rawdata[index];
  }
  return 0;
};


Int_t THaScaler::GetScaler(const char* detector, Int_t chan) {
  return GetScaler(detector, "LR", chan);
};

Int_t THaScaler::GetScaler(const char* det, const char* pm, Int_t chan, 
			   Int_t histor) 
{
// Accum. counts on PMTs of detector = "s1", "s2", "gasc", "a1", "a2",
// "leadgl", "edtm".   PMT = "left" or "right" or "LR"    
  string detector = det;
  string PMT = pm;
  if ( !did_init | !one_load ) return 0;
  if ( !database ) return 0;
  if ( database->FindNoCase(PMT,"Left") != std::string::npos ) detector += "L";
  if ( database->FindNoCase(PMT,"Right") != std::string::npos ) detector += "R";
  Int_t slot = GetSlot(detector);
  if (slot == -1) return 0;
  return GetScaler(slot,GetChan(detector,0,chan),histor);
};
  
Int_t THaScaler::GetTrig(Int_t trigger) {
// Non-helicity gated trigger counts for trig# 1,2,3...
   return GetTrig(0, trigger);
};

Int_t THaScaler::GetTrig(Int_t helicity, Int_t trig, Int_t histor) {
// Accum. counts for trig# 1,2,3,4,5, etc
// by helcity state (-1, 0, +1)  where 0 is non-helicity gated
// histor = 0 = this event.  1 = previous event.
  char ctrig[50];
  sprintf(ctrig,"trigger-%d",trig);
  return GetNormData(helicity, ctrig, histor);
};

Int_t THaScaler::GetBcm(const char* which) {
// Non-helicity gated BCM counts for 'which' = 'bcm_u1','bcm_d1',
// 'bcm_u3','bcm_d3','bcm_u10','bcm_d10'
   return GetBcm(0, which);
};

Int_t THaScaler::GetBcm(Int_t helicity, const char* which, Int_t histor) {
// Get BCM counts for 'which' = 'bcm_u1','bcm_d1',
// 'bcm_u3','bcm_d3','bcm_u10','bcm_d10'
// by helcity state (-1, 0, +1)  where 0 is non-helicity gated
// histor = 0 = this event.  1 = previous event.
   return GetNormData(helicity, which, histor);
};

Int_t THaScaler::GetPulser(const char* which) {
   return GetPulser(0, which);
};

Int_t THaScaler::GetPulser(Int_t helicity, const char* which, Int_t histor) {
// Obtain pulser values, by 'which' = 'clock', 'edt', 'edtat', 'strobe', etc
  return GetNormData(helicity, which, histor);
};   

Int_t THaScaler::GetNormData(Int_t helicity, const char* which, Int_t histor) {
// Get Normalization Data for channel 'which'
// by helcity state (-1, 0, +1)  where 0 is non-helicity gated
// histor = 0 = this event.  1 = previous event.
  if ( !did_init | !one_load ) return 0;
  Int_t index = 0;
  if (helicity == -1) index = 1;
  if (helicity ==  1) index = 2;
  if (normslot[index] < 0) return 0;
  std::multimap<std::string, Int_t>::iterator pm = normmap.find(which);
  if (pm == normmap.end()) return 0;
  return GetScaler(normslot[index],pm->second,histor);
};

Int_t THaScaler::GetNormData(Int_t helicity, Int_t chan, Int_t histor) {
// Get Normalization Data for channel #chan = 0,1,2,...
// by helcity state (-1, 0, +1)  where 0 is non-helicity gated
// histor = 0 = this event.  1 = previous event.
// Assumption: a slot with "TS-accept" is a normalization scaler.
  if ( !did_init | !one_load ) return 0;
  Int_t index = 0;
  if (helicity == -1) index = 1;
  if (helicity ==  1) index = 2;
  if (normslot[index] == -1) return 0;
  return GetScaler(normslot[index],chan,histor);  
};

Int_t THaScaler::GetSlot(string detector, Int_t helicity) {
  if (!database) return -1;
  return database->GetSlot(crate, detector, helicity);
};

Int_t THaScaler::GetChan(string detector, Int_t helicity, Int_t chan) {
  if (!database) return 0;
  return database->GetChan(crate, detector, helicity, chan);
};

Double_t THaScaler::GetScalerRate(Int_t slot, Int_t chan) {
// Rates on scaler data, for slot #slot, channel #chan
  Double_t rate,etime;
  etime = GetTimeDiff(0);
  if (etime > 0) { 
      rate = ( GetScaler(slot, chan, 0) -
               GetScaler(slot, chan, 1) ) / etime;
      return rate;
  }
  return 0;
};

Double_t THaScaler::GetScalerRate(const char* detector, Int_t chan) {
   return GetScalerRate(detector, "LR", chan);
};

Double_t THaScaler::GetScalerRate(const char* detector, const char* PMT, Int_t chan) {
// Rates (Hz) since last update for detector 's1', 's2', 'gasC', etc
// PMT = "left", "right", "LR" (coinc),  and channel #chan.
  Double_t rate,etime;
  etime = GetTimeDiff(0);
  if (etime > 0) { 
      rate = ( GetScaler(detector, PMT, chan, 0) -
               GetScaler(detector, PMT, chan, 1) ) / etime;
      return rate;
  }
  return 0;
};

Double_t THaScaler::GetTrigRate(Int_t trigger) {
  return GetTrigRate(0, trigger);
};

Double_t THaScaler::GetTrigRate(Int_t helicity, Int_t trigger) {
  Double_t rate,etime;
  etime = GetTimeDiff(helicity);
  if (etime > 0) { 
      rate = ( GetTrig(helicity, trigger, 0) -
               GetTrig(helicity, trigger, 1) ) / etime;
      return rate;
  }
  return 0;
};

Double_t THaScaler::GetBcmRate(const char* which) {
  return GetBcmRate(0, which);
};      

Double_t THaScaler::GetBcmRate(Int_t helicity, const char* which) {
  Double_t rate,etime;
  etime = GetTimeDiff(helicity);
  if (etime > 0) { 
      rate = ( GetBcm(helicity, which, 0) -
               GetBcm(helicity, which, 1) ) / etime;
      return rate;
  }
  return 0;
};

Double_t THaScaler::GetPulserRate(const char* which) {
  return GetPulserRate(0, which);
};   

Double_t THaScaler::GetPulserRate(Int_t helicity, const char* which) {
  Double_t rate,etime;
  etime = GetTimeDiff(helicity);
  if (etime > 0) { 
      rate = ( GetPulser(helicity, which, 0) -
               GetPulser(helicity, which, 1) ) / etime;
      return rate;
  }
  return 0;
};

Double_t THaScaler::GetNormRate(Int_t helicity, const char* which) {
  Double_t rate,etime;
  etime = GetTimeDiff(helicity);
  if (etime > 0) { 
      rate = ( GetNormData(helicity, which, 0) -
               GetNormData(helicity, which, 1) ) / etime;
      return rate;
  }
  return 0;
};

Double_t THaScaler::GetNormRate(Int_t helicity, Int_t chan) {
  Double_t rate,etime;
  etime = GetTimeDiff(helicity);
  if (etime > 0) { 
      rate = ( GetNormData(helicity, chan, 0) -
               GetNormData(helicity, chan, 1) ) / etime;
      return rate;
  }
  return 0;
};

Double_t THaScaler::GetTimeDiff(Int_t helicity) {
// Get the time difference in seconds.  Must normalize to a clock.
// If we have "SetClockLoc" then we use the location it defined.
// Otherwise we use the clock location from the "clock" item in
// the scaler.map file (which is the usual way).
  if ( !use_clock ) {
    Double_t etime = 0;
    if (clockrate != 0) etime = 1/clockrate;
    return etime;
  }
  if (clockrate == 0) return 0;
  if (clkslot != -1 && clkchan != -1) {
    return ((GetScaler(clkslot, clkchan, 0) -
             GetScaler(clkslot, clkchan, 1)) /
  	          clockrate);
  } else {
    return ((GetNormData(helicity,"clock",0) -
	     GetNormData(helicity,"clock",1)) /
            	  clockrate) ;
  }
};

UInt_t THaScaler::header_str_to_base16(string hdr) {
// Utility to convert string header to base 16 integer
  static bool hs16_first = true;
  static map<char, int> strmap;
  typedef map<char,int>::value_type valType;
  //  pair<char, int> pci;
  static char chex[]="0123456789abcdef";
  static vector<int> numarray; 
  static int linesize = 12;
  if (hs16_first) {
    hs16_first = false;
    for (int i = 0; i < 16; i++) {
      strmap.insert(valType(chex[i],i));
    }
    numarray.reserve(linesize);
  }
  numarray.clear();
  for (string::iterator p = hdr.begin(); p != hdr.end(); p++) {
    map<char, int>::iterator pm = strmap.find(*p);     
    if (pm != strmap.end()) numarray.push_back(pm->second);
    if ((long)numarray.size() > linesize-1) break;
  }
  UInt_t result = 0;  UInt_t power = 1;
  for (vector<int>::reverse_iterator p = numarray.rbegin(); 
      p != numarray.rend(); p++) {
      result += (*p) * power;  power *= 16;
  }
  return result;
};

void THaScaler::DumpRaw(Int_t flag)
{
  int size = SCAL_NUMBANK*SCAL_NUMCHAN;
  cout << "Raw data dump, flag "<<flag<<"   size "<<size<<endl;
  for (int i = 0; i < 10; i++) {
    cout << "rawdata["<<i<<"] = "<<rawdata[i]<<"   previous = "<<rawdata[i+size]<<endl;
  }
};


Int_t THaScaler::CheckInit() {
  if ( !did_init ) {
    if (SCAL_VERBOSE) {
      cout << "WARNING: THaScaler: Unitialized THaScaler object"<<endl;
      cout << "Likely errors are:"<<endl;
      cout << "   1. User did not call Init() method"<<endl;
      cout << "   2. scaler.map file not found "<<endl;
      cout << "(scaler.map is on web and also in scaler source dir)"<<endl;
    }
    return SCAL_ERROR;
  }
  return 0;
};

void THaScaler::Clear(Option_t* opt) {
   memset(rawdata,0,SCAL_NUMBANK*SCAL_NUMCHAN*sizeof(Int_t));
};

void THaScaler::ClearAll() {
  memset(rawdata,0,2*SCAL_NUMBANK*SCAL_NUMCHAN*sizeof(Int_t));
};

void THaScaler::LoadPrevious() {
  int size = SCAL_NUMBANK*SCAL_NUMCHAN;
  for (int i = 0; i < size; i++) {
    rawdata[i+size] = rawdata[i];
  }
};

ClassImp(THaScaler)

