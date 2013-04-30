/**************************************************************************
 * Copyright(c) 1998-1999, ALICE Experiment at CERN, All rights reserved. *
 *                                                                        *
 * Author: The ALICE Off-line Project.                                    *
 * Contributors are mentioned in the code where appropriate.              *
 *                                                                        *
 * Permission to use, copy, modify and distribute this software and its   *
 * documentation strictly for non-commercial purposes is hereby granted   *
 * without fee, provided that the above copyright notice appears in all   *
 * copies and that both the copyright notice and this permission notice   *
 * appear in the supporting documentation. The authors make no claims     *
 * about the suitability of this software for any purpose. It is          *
 * provided "as is" without express or implied warranty.                  *
 **************************************************************************/

//
// AliMuonAccEffSubmitter : a class to help submit Acc x Eff simulations
// anchored to real runs for J/psi, upsilon, single muons, etc...
//
// This class is dealing with 3 different directories :
//
// - template directory ($ALICE_ROOT/PWG/muondep/AccEffTemplates) containing the
//   basic template files to be used for a simuation. A template can contain
//   some variables that will be replaced during during the copy from template
//   to local dir
//
// - local directory, where the files from the template directory, are copied
//   once the class has been configured properly (i.e. using the various Set, Use,
//   etc... methods). Some other files (e.g. JDL ones) are generated from
//   scratch and also copied into this directory.
//   At this point one could(should) check the files, as they are the ones
//   to be copied to the remote directory for the production
//
// - remote directory, the alien directory where the files will be copied
//   (from the local directory) before the actual submission
//
// ==========================================================
//
// Basic usage
//
// AliMuonAccEffSubmitter a;
// a.UseOCDBSnapshots(kFALSE);
// a.SetRemoteDir("/alice/cern.ch/user/l/laphecet/Analysis/LHC13d/simjpsi/pp503z0");
// a.ShouldOverwriteFiles(true);
// a.MakeNofEventsPropToTriggerCount("CMUL7-B-NOPF-MUON");
// a.SetVar("VAR_GENLIB_PARNAME","\"pp 5.03\"");
// a.SetRunList(195682);
// a.Print();
// a.Run("test"); // will do everything but the submit
// a.Submit(false); // actual submission
//
//
// author: Laurent Aphecetche (Subatech
//

#include "AliMuonAccEffSubmitter.h"

#include "AliAnalysisTriggerScalers.h"
#include "AliLog.h"
#include "TFile.h"
#include "TGrid.h"
#include "TGridResult.h"
#include "TMap.h"
#include "TMath.h"
#include "TObjString.h"
#include "TROOT.h"
#include "TString.h"
#include "TSystem.h"
#include <vector>
#include <fstream>
using std::ifstream;
namespace
{
  Int_t splitLevel=10;
}

//______________________________________________________________________________
AliMuonAccEffSubmitter::AliMuonAccEffSubmitter(const char* generator)
: TObject(),
fScalers(0x0),
fRemoteDir(""),
fReferenceTrigger(""),
fRatio(1.0),
fFixedNofEvents(10000),
fMaxEventsPerChunk(5000),
fLocalDir(gSystem->pwd()),
fOCDBPath("raw://"),
fTemplateDir(gSystem->ExpandPathName("$ALICE_ROOT/PWG/muondep/AccEffTemplates")),
fPackageAliroot(),
fPackageGeant3(),
fPackageRoot(),
fPackageApi(),
fMergedDir(Form("%s/AODs",fRemoteDir.Data())),
fSplitMaxInputFileNumber(20),
fCompactMode(1),
fShouldOverwriteFiles(kFALSE),
fVars(0x0),
fExternalConfig(""),
fUseOCDBSnapshots(kTRUE),
fIsValid(kFALSE),
fTemplateFileList(0x0),
fLocalFileList(0x0),
fSnapshotDir(fLocalDir),
fUseAODMerging(kFALSE)
{
  // ctor
  
  if (!TGrid::Connect("alien://"))
  {
    AliError("cannot connect to grid");
    fIsValid = kFALSE;
  }

  SetPackages("VO_ALICE@AliRoot::v5-03-Rev-18","VO_ALICE@GEANT3::v1-14-8","VO_ALICE@ROOT::v5-34-05-1");
  
  SetVar("VAR_OCDB_PATH","\"raw://\"");

  SetVar("VAR_GENPARAM_GENLIB_TYPE","AliGenMUONlib::kJpsi");
  SetVar("VAR_GENPARAM_GENLIB_PARNAME","\"pPb 5.03\"");

  SetVar("VAR_GENCORRHF_QUARK","5");
  SetVar("VAR_GENCORRHF_ENERGY","5");

  // some default values for J/psi
  SetVar("VAR_GENPARAMCUSTOM_PDGPARTICLECODE","443");

  // default values below are from J/psi p+Pb (from muon_calo pass)
  SetVar("VAR_GENPARAMCUSTOM_Y_P0","4.08E5");
  SetVar("VAR_GENPARAMCUSTOM_Y_P1","7.1E4");
  
  SetVar("VAR_GENPARAMCUSTOM_PT_P0","1.13E9");
  SetVar("VAR_GENPARAMCUSTOM_PT_P1","18.05");
  SetVar("VAR_GENPARAMCUSTOM_PT_P2","2.05");
  SetVar("VAR_GENPARAMCUSTOM_PT_P3","3.34");

  // some default values for single muons
  SetVar("VAR_GENPARAMCUSTOMSINGLE_PTMIN","0.35");
  
  SetVar("VAR_GENPARAMCUSTOMSINGLE_PT_P0","4.05962");
  SetVar("VAR_GENPARAMCUSTOMSINGLE_PT_P1","1.0");
  SetVar("VAR_GENPARAMCUSTOMSINGLE_PT_P2","2.46187");
  SetVar("VAR_GENPARAMCUSTOMSINGLE_PT_P3","2.08644");

  SetVar("VAR_GENPARAMCUSTOMSINGLE_Y_P0","0.729545");
  SetVar("VAR_GENPARAMCUSTOMSINGLE_Y_P1","0.53837");
  SetVar("VAR_GENPARAMCUSTOMSINGLE_Y_P2","0.141776");
  SetVar("VAR_GENPARAMCUSTOMSINGLE_Y_P3","0.0130173");

  UseOCDBSnapshots(kTRUE);
  
  SetGenerator(generator);
}

//______________________________________________________________________________
AliMuonAccEffSubmitter::~AliMuonAccEffSubmitter()
{
  // dtor
  delete fScalers;
  delete fTemplateFileList;
  delete fLocalFileList;
  delete fVars;
}

//______________________________________________________________________________
Bool_t AliMuonAccEffSubmitter::CheckCompilation(const char* file) const
{
  /// Check whether file can be compiled or not
  /// FIXME: use gSystem->TempFileName for tmpfile !
  
  Bool_t rv(kTRUE);
  
  TString sfile(gSystem->BaseName(file));
  TString tmpfile(Form("tmpfile_%s",sfile.Data()));
  
  gSystem->Exec(Form("cp %s %s",file,tmpfile.Data()));
  
  ReplaceVars(tmpfile.Data());
  
  gSystem->AddIncludePath("-I$ALICE_ROOT/include");
  gSystem->AddIncludePath("-I$ALICE_ROOT/EVGEN");

  if (gROOT->LoadMacro(Form("%s++",tmpfile.Data())))
  {
    AliError(Form("macro %s can not be compiled. Please check.",file));
    rv = kFALSE;
  }
  
  gSystem->Exec(Form("rm %s",tmpfile.Data()));
  
  return rv;
}


//______________________________________________________________________________
Bool_t AliMuonAccEffSubmitter::CheckLocal() const
{
  /// Check whether all required local files are there
  TIter next(LocalFileList());
  TObjString* file;
  
  while ( ( file = static_cast<TObjString*>(next())) )
  {
      if ( gSystem->AccessPathName(file->String().Data()) )
      {
        return kFALSE;
      }
  }
  
  return kTRUE;
}

//______________________________________________________________________________
Bool_t AliMuonAccEffSubmitter::CheckRemote() const
{
  /// Check whether all required remote files are there
  AliWarning("implement me");
  return kFALSE;
}

//______________________________________________________________________________
void AliMuonAccEffSubmitter::CleanLocal(Bool_t cleanSnapshots) const
{
  /// Clean (remove) local generated files
  /// As OCDB snapshot creation is a long process, cleanSnapshots
  /// is kFALSE by default in order not to delete those.
  
  TIter next(LocalFileList());
  TObjString* file;
  
  while ( ( file = static_cast<TObjString*>(next())) )
  {
    if ( !cleanSnapshots && file->String().Contains("OCDB_") ) continue;
    gSystem->Unlink(file->String().Data());
  }
}

//______________________________________________________________________________
void AliMuonAccEffSubmitter::CleanRemote() const
{
  /// Clean (remove) remote files
  AliWarning("implement me");
}

//______________________________________________________________________________
Bool_t AliMuonAccEffSubmitter::CopyFile(const char* localFile)
{
  /// copy a local file to remote destination
  TString local;
  
  if ( gSystem->IsAbsoluteFileName(localFile) )
  {
    local = localFile;
  }
  else
  {
    local = Form("%s/%s",fLocalDir.Data(),gSystem->ExpandPathName(localFile));
  }
  
  if (gSystem->AccessPathName(local.Data()))
  {
    AliError(Form("Local file %s does not exist",local.Data()));
    return kFALSE;
  }
  
  TString remote;
  
  remote += fRemoteDir;
  remote += "/";
  
  if ( gSystem->IsAbsoluteFileName(localFile) )
  {
    TString tmp(localFile);
    tmp.ReplaceAll(fSnapshotDir.Data(),"");
    remote += tmp;
  }
  else
  {
    remote += localFile;
  }
  
  TString dirName = gSystem->DirName(remote.Data());
  
  Bool_t ok(kTRUE);
  
  if (!RemoteDirectoryExists(dirName.Data()))
  {
    ok = gGrid->Mkdir(dirName.Data(),"-p");
  }
  
  if ( ok )
  {
    AliDebug(1,Form("cp %s alien://%s",local.Data(),remote.Data()));
    return TFile::Cp(local.Data(),Form("alien://%s",remote.Data()));
  }
  else
  {
    return kFALSE;
  }
}

//______________________________________________________________________________
Bool_t AliMuonAccEffSubmitter::CheckRemoteDir() const
{
  /// Check we have a grid connection and that the remote dir exists
  
  if (fRemoteDir.IsNull())
  {
    AliError("you must provide the grid location where to copy the files");
    return kFALSE;
  }
  
  // connect to alien
  if (!gGrid)
  {
    if (!TGrid::Connect("alien://"))
    {
      AliError("Cannot connect to grid");
      return kFALSE;
    }
  }
  
  if (!RemoteDirectoryExists(fRemoteDir))
  {
    AliError(Form("directory %s does not exist", fRemoteDir.Data()));
    return kFALSE;
  }

  return kTRUE;
}

//______________________________________________________________________________
Bool_t AliMuonAccEffSubmitter::CopyLocalFilesToRemote()
{
  /// copy all files necessary to run the simulation into remote directory
  
  if (!IsValid()) return kFALSE;
  
  AliDebug(1,"");
  
  if ( CheckRemoteDir() )
  {
    TString sdir(gSystem->ExpandPathName(LocalDir()));
  
    TIter next(LocalFileList());
    TObjString* ftc;
  
    Bool_t allok(kTRUE);
  
    while ( ( ftc = static_cast<TObjString*>(next())) )
    {
      allok = allok && CopyFile(ftc->String());
    }
    return allok;
  }
  
  return kFALSE;
}

//______________________________________________________________________________
Bool_t AliMuonAccEffSubmitter::CopyTemplateFilesToLocal()
{
  // copy (or generate) local files from the template ones
  
  if (!IsValid()) return kFALSE;

  AliDebug(1,"");

  TIter next(TemplateFileList());
  TObjString* file;
  
  Int_t err(0);
  Bool_t potentialProblem(kFALSE);
  
  while ( ( file = static_cast<TObjString*>(next())) )
  {
    if ( file->String().Contains("OCDB") )
    {
      /// OCDB snapshots are not in template
      continue;
    }

    if ( !ShouldOverwriteFiles() && !gSystem->AccessPathName(file->String().Data()) )
    {
      AliError(Form("Local file %s already exists. Remove it first if you want to update overwrite it",file->String().Data()));
      potentialProblem = kTRUE;
    }
    else
    {
      TString stemplate(Form("%s/%s",fTemplateDir.Data(),file->String().Data()));
      TString slocal(Form("%s/%s",fLocalDir.Data(),file->String().Data()));
      
      Int_t c =  gSystem->CopyFile(stemplate.Data(),slocal.Data(),ShouldOverwriteFiles());
      if ( c )
      {
        Bool_t ok(kFALSE);
        if ( stemplate.Contains(".jdl",TString::kIgnoreCase) )
        {
          if ( stemplate.Contains("merge",TString::kIgnoreCase) )
          {
            ok = GenerateMergeJDL(file->String().Data());
          }
          else
          {
            ok = GenerateRunJDL(file->String().Data());
          }
        }
        if (!ok)
        {
          AliError(Form("Error %d copying file %s",c,stemplate.Data()));
        }
        else
        {
          c=0;
        }
      }
      else
      {
        if ( HasVars(slocal.Data()) )
        {
          if (!ReplaceVars(slocal.Data()))
          {
            AliError("pb in ReplaceVars");
            c=1;
          }
        }
      }
      err += c;
    }
  }
  
  if ( potentialProblem )
  {
    AliWarning("At least one local file could not be overwritten. Cross-check that the local files are OK before we try to upload them to the Grid !");
    return kFALSE;
  }
  return (err==0);
}

//______________________________________________________________________________
std::ostream* AliMuonAccEffSubmitter::CreateJDLFile(const char* name) const
{
  /// Create a JDL file
  AliDebug(1,"");

  TString jdl(Form("%s/%s",fLocalDir.Data(),name));
  
  if ( !ShouldOverwriteFiles() && !gSystem->AccessPathName(jdl.Data()) )
  {
    AliError(Form("File %s already exists. Remove it if you want to overwrite it",jdl.Data()));
    return 0x0;
  }
  
  std::ofstream* os = new std::ofstream(gSystem->ExpandPathName(jdl.Data()));
  
  if (os->bad())
  {
    AliError(Form("Cannot create file %s",jdl.Data()));
    delete os;
    os=0x0;
  }
  
  return os;
}

///______________________________________________________________________________
Bool_t AliMuonAccEffSubmitter::GenerateMergeJDL(const char* name)
{
  /// Create the JDL for merging jobs
  /// FIXME: not checked !
  
  AliDebug(1,"");

  std::ostream* os = CreateJDLFile(name);
  
  if (!os)
  {
    return kFALSE;
  }
  
  Bool_t final = TString(name).Contains("merge",TString::kIgnoreCase);

  (*os) << "# Generated merging jdl (production mode)" << std::endl
  << "# $1 = run number" << std::endl
  << "# $2 = merging stage" << std::endl
  << "# Stage_<n>.xml made via: find <OutputDir> *Stage<n-1>/*root_archive.zip" << std::endl;

  Output(*os,"Packages",fPackageAliroot.Data(),fPackageGeant3.Data(),
         fPackageRoot.Data(),fPackageApi.Data());
  
  Output(*os,"Executable","AOD_merge.sh");
  
  Output(*os,"Price","1");

  if ( final )
  {
    Output(*os,"Jobtag","comment: AliMuonAccEffSubmitter final merging");
  }
  else
  {
    Output(*os,"Jobtag","comment: AliMuonAccEffSubmitter merging stage $2");
  }
  
  Output(*os,"Workdirectorysize","5000MB");
  
  Output(*os,"Validationcommand",Form("%s/validation_merge.sh",fRemoteDir.Data()));
  
  Output(*os,"TTL","7200");

  Output(*os,"OutputArchive",
    "log_archive.zip:stderr,stdout@disk=1",
    "root_archive.zip:AliAOD.root,AliAOD.Muons.root,AnalysisResults.root@disk=3"
         );
  
  Output(*os,"Arguments",(final ? "2":"1")); // for AOD_merge.sh, 1 means intermediate merging stage, 2 means final merging
  
  if ( !final )
  {
    Output(*os,"InputFile",Form("LF:%s/AODtrain.C",fRemoteDir.Data()));
    Output(*os,"OutputDir",Form("%s/$1/Stage_$2/#alien_counter_03i#",fRemoteDir.Data()));
    Output(*os,"InputDataCollection",Form("%s/$1/Stage_$2.xml,nodownload",fRemoteDir.Data()));
    Output(*os,"split","se");
    Output(*os,"SplitMaxInputFileNumber",GetSplitMaxInputFileNumber());
    Output(*os,"InputDataListFormat","xml-single");
    Output(*os,"InputDataList","wn.xml");
  }
  else
  {
    Output(*os,"InputFile",Form("LF:%s/AODtrain.C",fRemoteDir.Data()),
           Form("LF:%s/$1/wn.xml",fRemoteDir.Data()));
    Output(*os,"OutputDir",Form("%s/$1",fRemoteDir.Data()));
  }
  
  return kTRUE;
}

//______________________________________________________________________________
Bool_t AliMuonAccEffSubmitter::GenerateRunJDL(const char* name)
{
  /// Generate (locally) the JDL to perform the simulation+reco+aod filtering
  /// (to be then copied to the grid and finally submitted)
  
  AliDebug(1,"");

  std::ostream* os = CreateJDLFile(name);
  
  if (!os)
  {
    return kFALSE;
  }
  
  Output(*os,"Packages",fPackageAliroot.Data(),fPackageGeant3.Data(),
         fPackageRoot.Data(),fPackageApi.Data());

  Output(*os,"Jobtag","comment: AliMuonAccEffSubmitter RUN $1");

  Output(*os,"split","production:1-$2");

  Output(*os,"Price","1");
  
  Output(*os,"OutputDir",Form("%s/$1/#alien_counter_03i#",fRemoteDir.Data()));

  Output(*os,"Executable","/alice/bin/aliroot_new");
  
  TObjArray files;
  files.SetOwner(kTRUE);
  TIter next(TemplateFileList());
  TObjString* file;
  
  while ( ( file = static_cast<TObjString*>(next())) )
  {
    if ( !file->String().Contains(".jdl",TString::kIgnoreCase) ||
         !file->String().Contains("OCDB_") )
    {
      files.Add(new TObjString(Form("LF:%s/%s",fRemoteDir.Data(),file->String().Data())));      
    }
  }
  
  if ( fUseOCDBSnapshots )
  {
    files.Add(new TObjString(Form("LF:%s/OCDB/$1/OCDB_sim.root",fRemoteDir.Data())));
    files.Add(new TObjString(Form("LF:%s/OCDB/$1/OCDB_rec.root",fRemoteDir.Data())));
  }
  
  Output(*os,"InputFile",files);
  
  if ( CompactMode() == 0 )
  {
    // store everything
    Output(*os,"OutputArchive",  "log_archive.zip:stderr,stdout,aod.log,checkaod.log,checkesd.log,rec.log,sim.log@disk=1",
           "root_archive.zip:galice*.root,Kinematics*.root,TrackRefs*.root,AliESDs.root,AliAOD.root,AliAOD.Muons.root,Merged.QA.Data.root,Run*.root@disk=2");
  }
  else if ( CompactMode() == 1 )
  {
    // keep only muon AODs
    Output(*os,"OutputArchive",  "log_archive.zip:stderr,stdout,aod.log,checkaod.log,checkesd.log,rec.log,sim.log@disk=1",
           "root_archive.zip:galice*.root,AliAOD.Muons.root@disk=2");
  }
  else
  {
    AliError(Form("Unknown CompactMode %d",CompactMode()));
    delete os;
    return kFALSE;
  }
  
  Output(*os,"splitarguments","simrun.C --run $1 --chunk #alien_counter# --event $3");
  
  Output(*os,"Workdirectorysize","5000MB");
  
  Output(*os,"JDLVariables","Packages","OutputDir");

  Output(*os,"Validationcommand",Form("%s/validation.sh",fRemoteDir.Data()));

  Output(*os,"TTL","72000");
  
  return kTRUE;
}

//______________________________________________________________________________
Bool_t AliMuonAccEffSubmitter::GetLastStage(const char* remoteDir) const
{
  /// Get the last staging phase already performed
  /// FIXME : not checked !
  
  Int_t n = 0, lastStage = 0;
  gSystem->Exec(Form("alien_ls -F %s | grep Stage_.*/ > __stage__", remoteDir));
  ifstream f("__stage__");
  std::string dummy;
  while (std::getline(f, dummy)) n++;
  f.close();
  while (n > 0) if (gSystem->Exec(Form("grep Stage_%d/ __stage__ 2>&1 >/dev/null", ++lastStage)) == 0) n--;
  gSystem->Exec("rm -f __stage__");
  return lastStage;
}

//______________________________________________________________________________
TObjArray* AliMuonAccEffSubmitter::GetVariables(const char* file) const
{
  /// Find the variables in the file
  
  std::ifstream in(file);
  char line[1024];
  TObjArray* variables(0x0);
  
  while ( in.getline(line,1023,'\n') )
  {
    TString sline(line);
    while (sline.Contains("VAR_") && !sline.BeginsWith("//") )
    {
      Int_t i1 = sline.Index("VAR_");
      Int_t i2(i1);
      
      while ( ( i2 < sline.Length() ) && ( isalnum(sline[i2]) || sline[i2]=='_' ) ) ++i2;
      
      if (!variables)
      {
        variables = new TObjArray;
        variables->SetOwner(kTRUE);
      }
      
      TString var = sline(i1,i2-i1);
      if ( !variables->FindObject(var) )
      {
        variables->Add(new TObjString(var));
      }
      sline.ReplaceAll(var,"");
    }
  }
  
  in.close();
  
  return variables;
}

//______________________________________________________________________________
Bool_t AliMuonAccEffSubmitter::HasVars(const char* file) const
{
  /// Whether or not the file contains variables that have to
  /// be substituted
  
  std::ifstream in(file);
  char line[1024];
  while ( in.getline(line,1023,'\n') )
  {
    TString sline(line);
    if (sline.Contains("VAR_") && !sline.BeginsWith("//") )
    {
      return kTRUE;
    }
  }
  return kFALSE;
}

//______________________________________________________________________________
TObjArray* AliMuonAccEffSubmitter::LocalFileList() const
{
  /// Return (after createing and filling it if needed)
  /// the internal file list with paths from the local directory
  
  if (!fLocalFileList)
  {
    fLocalFileList = static_cast<TObjArray*>(TemplateFileList()->Clone());
  }
  
  return fLocalFileList;
}

//______________________________________________________________________________
Bool_t AliMuonAccEffSubmitter::MakeOCDBSnapshots()
{
  /// Run sim.C and rec.C in a special mode to generate OCDB snapshots
  /// Can only be done after the templates have been copied locally
  
  if (!IsValid()) return kFALSE;

  if (!fUseOCDBSnapshots) return kTRUE;
  
  if (!fScalers) return kFALSE;
  
  AliDebug(1,"");

  const std::vector<int>& runs = fScalers->GetRunList();

  Bool_t ok(kTRUE);
  
  for ( std::vector<int>::size_type i = 0; i < runs.size(); ++i )
  {
    Int_t runNumber = runs[i];

    TString ocdbSim(Form("%s/OCDB/%d/OCDB_sim.root",SnapshotDir().Data(),runNumber));
    TString ocdbRec(Form("%s/OCDB/%d/OCDB_rec.root",SnapshotDir().Data(),runNumber));

    if ( !gSystem->AccessPathName(ocdbSim.Data()) &&
         !gSystem->AccessPathName(ocdbRec.Data()) )
    {
      AliWarning(Form("Local OCDB snapshots already there for run %d. Will not redo them. If you want to force them, delete them by hand !",runNumber));
      continue;
    }
    else
    {
      gSystem->Exec(Form("aliroot -b -q -x simrun.C --run %d --snapshot",runNumber));
    
      if ( gSystem->AccessPathName(ocdbSim.Data()) )
      {
        AliError(Form("Could not create OCDB snapshot for simulation"));
        ok = kFALSE;
      }

      if ( gSystem->AccessPathName(ocdbRec.Data()) )
      {
        AliError(Form("Could not create OCDB snapshot for reconstruction"));
        ok = kFALSE;
      }
    }
    
    LocalFileList()->Add(new TObjString(ocdbSim));
    LocalFileList()->Add(new TObjString(ocdbRec));
  }
  
  return ok;
}

//______________________________________________________________________________
Bool_t AliMuonAccEffSubmitter::Merge(Int_t stage, Bool_t dryRun)
{
  /// Submit multiple merging jobs with the format "submit AOD_merge(_final).jdl run# (stage#)".
  /// Also produce the xml collection before sending jobs
  /// Initial AODs will be taken from fRemoteDir/[RUNNUMBER] while the merged
  /// ones will be put into fMergedDir/AODs/[RUNNUMBER]
  ///
  /// Example:
  /// - inDir = "/alice/sim/2012/LHC12a10_bis" (where to find the data to merge)
  ///         = 0x0 --> inDir = homeDir/outDir/resDir
  /// - outDir = "Sim/LHC11h/embedding/AODs" (where to store merged results)
  /// - runList.txt must contains the list of run number
  /// - stage=0 --> final merging / stage>0 --> intermediate merging i
  ///
  
  if (!RemoteDirectoryExists(fMergedDir.Data())) {
    AliError(Form("directory %s does not exist", fMergedDir.Data()));
    return kFALSE;
  }
  
  gGrid->Cd(fMergedDir.Data());
  
  TString jdl = MergeJDLName(stage==0);
  
  if (!RemoteFileExists(jdl.Data()))
  {
    AliError(Form("file %s does not exist in %s\n", jdl.Data(), fRemoteDir.Data()));
    return kFALSE;
  }
  
  const std::vector<int>& runs = fScalers->GetRunList();
  
  if (runs.empty())
  {
    AliError("No run to work with");
    return kFALSE;
  }

  TString currRun;
  TString reply = "";
  gSystem->Exec("rm -f __failed__");
  Bool_t failedRun = kFALSE;
  
  for ( std::vector<int>::size_type i = 0; i < runs.size(); ++i )
  {
    Int_t run = runs[i];
    AliInfo(Form("\n --- processing run %d ---\n", run));
    
    TString runDir = Form("%s/%d", fMergedDir.Data(), run);
    
    if (!RemoteDirectoryExists(runDir.Data()))
    {
      AliInfo(Form(" - creating output directory %s\n", runDir.Data()));
      gSystem->Exec(Form("alien_mkdir -p %s", runDir.Data()));
    }
    
    if (RemoteFileExists(Form("%s/root_archive.zip", runDir.Data())))
    {
      AliWarning(" ! final merging already done");
      continue;
    }
    
    Int_t lastStage = GetLastStage(runDir.Data());
    
    if (stage > 0 && stage != lastStage+1)
    {
      AliError(Form(" ! lastest merging stage = %d. Next must be stage %d or final stage\n", lastStage, lastStage+1));
      continue;
    }
    
    TString wn = (stage > 0) ? Form("Stage_%d.xml", stage) : "wn.xml";
    TString find = (lastStage == 0) ?
    Form("alien_find -x %s %s/%d *root_archive.zip", wn.Data(), fRemoteDir.Data(), run) :
    Form("alien_find -x %s %s/%d/Stage_%d *root_archive.zip", wn.Data(), fRemoteDir.Data(), run, lastStage);
    gSystem->Exec(Form("%s 1> %s 2>/dev/null", find.Data(), wn.Data()));
    gSystem->Exec(Form("grep -c /event %s > __nfiles__", wn.Data()));
    ifstream f2("__nfiles__");
    TString nFiles;
    nFiles.ReadLine(f2,kTRUE);
    f2.close();
    gSystem->Exec("rm -f __nfiles__");
    printf(" - number of files to merge = %d\n", nFiles.Atoi());
    if (nFiles.Atoi() == 0) {
      printf(" ! collection of files to merge is empty\n");
      gSystem->Exec(Form("rm -f %s", wn.Data()));
      continue;
    } else if (stage > 0 && nFiles.Atoi() <= splitLevel && !reply.BeginsWith("y")) {
      if (!reply.BeginsWith("n")) {
        printf(" ! number of files to merge <= split level (%d). Continue? [Y/n] ", splitLevel);
        fflush(stdout);
        reply.Gets(stdin,kTRUE);
        reply.ToLower();
      }
      if (reply.BeginsWith("n")) {
        gSystem->Exec(Form("rm -f %s", wn.Data()));
        continue;
      } else reply = "y";
    }
    
    if (!dryRun)
    {
      TString dirwn = Form("%s/%s", runDir.Data(), wn.Data());
      if (RemoteFileExists(dirwn.Data())) gGrid->Rm(dirwn.Data());
      gSystem->Exec(Form("alien_cp file:%s alien://%s", wn.Data(), dirwn.Data()));
      gSystem->Exec(Form("rm -f %s", wn.Data()));
    }
    
    TString query;
    if (stage > 0) query = Form("submit %s %d %d", jdl.Data(), run, stage);
    else query = Form("submit %s %d", jdl.Data(), run);
    printf(" - %s ...", query.Data());
    fflush(stdout);
    
    if (dryRun)
    {
      AliInfo(" dry run");
      continue;
    }
    
    Bool_t done = kFALSE;
    TGridResult *res = gGrid->Command(query);
    if (res)
    {
      TString cjobId1 = res->GetKey(0,"jobId");
      if (!cjobId1.IsDec())
      {
        AliError(" FAILED");
        gGrid->Stdout();
        gGrid->Stderr();
      }
      else
      {
        AliInfo(Form(" DONE\n   --> the job Id is: %s \n", cjobId1.Data()));
        done = kTRUE;
      }
      delete res;
    }
    else
    {
      AliError(" FAILED");
    }
    
    if (!done)
    {
      gSystem->Exec(Form("echo %d >> __failed__", run));
      failedRun = kTRUE;
    }
    
  }
  
  if (failedRun)
  {
    AliInfo("\n--------------------\n");
    AliInfo("list of failed runs:\n");
    gSystem->Exec("cat __failed__");
    gSystem->Exec("rm -f __failed__");
    return kFALSE;
  }
  
  return kTRUE;
}

//______________________________________________________________________________
UInt_t AliMuonAccEffSubmitter::NofRuns() const
{
    // number of runs we're dealing with
  if (!fScalers) return 0;
  
  return fScalers->GetRunList().size();
}

//______________________________________________________________________________
void AliMuonAccEffSubmitter::Output(std::ostream& out, const char* key,
                                    const TObjArray& values) const
{
  /// output to ostream of key,{values} pair
  
  out << key << " = ";
  
  Int_t n = values.GetEntries();
  
  if ( n > 1 )
  {
    out << "{" << std::endl;
    TIter next(&values);
    TObjString* v;
    
    while ( ( v = static_cast<TObjString*>(next())) )
    {
      --n;
      out << "\t\"" << v->String().Data() << "\"";
      if  ( n ) out << ",";
      out << std::endl;
    }
    out << "}";
  }
  else
  {
    TString& v1 = static_cast<TObjString*>(values.At(0))->String();
    
    if ( v1.IsDigit() )
    {
      out << v1.Atoi();
    }
    else
    {
      out << "\"" << v1.Data() << "\"";
    }
  }
  out << ";" << std::endl;
}

//______________________________________________________________________________
void AliMuonAccEffSubmitter::Output(std::ostream& out, const char* key, const char* v1,
                                    const char* v2, const char* v3, const char* v4,
                                    const char* v5, const char* v6, const char* v7,
                                    const char* v8, const char* v9) const
{
  /// output to ostream
  
  TObjArray values;
  values.SetOwner(kTRUE);
  
  values.Add(new TObjString(v1));
  if ( strlen(v2) > 0 ) values.Add(new TObjString(v2));
  if ( strlen(v3) > 0 ) values.Add(new TObjString(v3));
  if ( strlen(v4) > 0 ) values.Add(new TObjString(v4));
  if ( strlen(v5) > 0 ) values.Add(new TObjString(v5));
  if ( strlen(v6) > 0 ) values.Add(new TObjString(v6));
  if ( strlen(v7) > 0 ) values.Add(new TObjString(v7));
  if ( strlen(v8) > 0 ) values.Add(new TObjString(v8));
  if ( strlen(v9) > 0 ) values.Add(new TObjString(v9));
  
  Output(out,key,values);
}


//______________________________________________________________________________
void AliMuonAccEffSubmitter::Print(Option_t* /*opt*/) const
{
  /// Printout
  
  if (!IsValid())
  {
    std::cout << std::string(80,'*') << std::endl;
    std::cout << "INVALID OBJECT. CHECK BELOW THE CONFIGURATION." << std::endl;
    std::cout << std::string(80,'*') << std::endl;
  }
    
  std::cout << "Template  directory = " << fTemplateDir.Data() << std::endl;
  std::cout << "Local     directory = " << fLocalDir.Data() << std::endl;
  std::cout << "Remote    directory = " << fRemoteDir.Data() << std::endl;
  
  if ( fSnapshotDir != fLocalDir )
  {
    std::cout << "Snapshots directory = " << fSnapshotDir.Data() << std::endl;
  }
  
  std::cout << "OCDB path = " << fOCDBPath.Data() << std::endl;
  
  if ( fRatio > 0 )
  {
    std::cout << Form("For each run, will generate %5.2f times the number of real events for trigger %s",
                      fRatio,fReferenceTrigger.Data()) << std::endl;
  }
  else
  {
    std::cout << Form("For each run, will generate %10d events",fFixedNofEvents) << std::endl;
  }
  
  std::cout << "MaxEventsPerChunk = " << fMaxEventsPerChunk << std::endl;
  
  if ( NofRuns() )
  {
    std::cout << NofRuns() << " run";
    if ( NofRuns() > 1 ) std::cout << "s";
    std::cout << " = ";
    fScalers->Print();
  }
  
  if ( fVars )
  {
    TIter next(fVars);
    TObjString* key;
    while ( ( key = static_cast<TObjString*>(next())) )
    {
      TObjString* value = static_cast<TObjString*>(fVars->GetValue(key->String()));
      std::cout << "Variable " << key->String() << " will be replaced by " << value->String() << std::endl;
    }
  }
  
  std::cout << "Files to be uploaded:" << std::endl;
  TIter nextFile(LocalFileList());
  TObjString* sfile;
  while ( ( sfile = static_cast<TObjString*>(nextFile())) )
  {
    std::cout << sfile->String().Data() << std::endl;
  }
}


//______________________________________________________________________________
Bool_t AliMuonAccEffSubmitter::RemoteDirectoryExists(const char *dirname) const
{
  // Returns true if directory exists. Can be also a path.
  if (!gGrid) return kFALSE;
  // Check if dirname is a path
  TString dirstripped = dirname;
  dirstripped = dirstripped.Strip();
  dirstripped = dirstripped.Strip(TString::kTrailing, '/');
  TString dir = gSystem->BaseName(dirstripped);
  dir += "/";
  TString path = gSystem->DirName(dirstripped);
  TGridResult *res = gGrid->Ls(path, "-F");
  if (!res) return kFALSE;
  TIter next(res);
  TMap *map;
  TObject *obj;
  while ((map=dynamic_cast<TMap*>(next()))) {
    obj = map->GetValue("name");
    if (!obj) break;
    if (dir == obj->GetName()) {
      delete res;
      return kTRUE;
    }
  }
  delete res;
  return kFALSE;
}

//______________________________________________________________________________
Bool_t AliMuonAccEffSubmitter::RemoteFileExists(const char *lfn) const
{
  // Returns true if file exists.
  if (!gGrid) return kFALSE;
  TGridResult *res = gGrid->Ls(lfn);
  if (!res) return kFALSE;
  TMap *map = dynamic_cast<TMap*>(res->At(0));
  if (!map) {
    delete res;
    return kFALSE;
  }
  TObjString *objs = dynamic_cast<TObjString*>(map->GetValue("name"));
  if (!objs || !objs->GetString().Length()) {
    delete res;
    return kFALSE;
  }
  delete res;
  return kTRUE;
}

//______________________________________________________________________________
Bool_t AliMuonAccEffSubmitter::ReplaceVars(const char* file) const
{
  /// Replace the variables (i.e. things starting by VAR_) found in file
  
  std::ifstream in(file);
  char line[1024];
  TObjArray lines;
  lines.SetOwner(kTRUE);
  Int_t nvars(0);
  Int_t nreplaced(0);

  TIter next(fVars);

  while ( in.getline(line,1023,'\n') )
  {
    TString sline(line);
    while (sline.Contains("VAR_") && !sline.BeginsWith("//") )
    {
      ++nvars;
      TObjString* key;
      next.Reset();
      while ( ( key = static_cast<TObjString*>(next())) )
      {
        if ( sline.Contains(key->String()) )
        {
          ++nreplaced;
          TObjString* value = static_cast<TObjString*>(fVars->GetValue(key->String()));
          sline.ReplaceAll(key->String(),value->String());
          break;
        }
      }
    }

    lines.Add(new TObjString(sline));
  }
  
  in.close();
  
  if ( nvars > 0 )
  {
    if ( nreplaced != nvars )
    {
      AliError(Form("nvars=%d nreplaced=%d",nvars,nreplaced));
      return kFALSE;
    }
    std::ofstream out(file);
    TIter nextLine(&lines);
    TObjString* s;
    while ( ( s = static_cast<TObjString*>(nextLine()) ) )
    {
      out << s->String().Data() << std::endl;
    }
    out.close();
  }
  
  return kTRUE;
}

//______________________________________________________________________________
Bool_t AliMuonAccEffSubmitter::Run(const char* mode)
{
  /// mode can be one of (case insensitive)
  ///
  /// LOCAL : copy the template files from the template directory to the local one
  /// UPLOAD : copy the local files to the grid (requires LOCAL)
  /// OCDB : make ocdb snapshots (requires LOCAL)
  /// SUBMIT : submit the jobs (requires LOCAL + UPLOAD)
  /// FULL : all of the above (requires all of the above)
  ///
  /// TEST : as SUBMIT, but in dry mode (does not actually submit the jobs)
  
  if (!IsValid()) return kFALSE;
  
  TString smode(mode);
  smode.ToUpper();
  
  if ( smode == "FULL")
  {
    return  ( Run("LOCAL") && Run("OCDB") && Run("UPLOAD") && Run("SUBMIT") );
  }
  
  if ( smode == "LOCAL")
  {
    return CopyTemplateFilesToLocal();
  }
  
  if ( smode == "UPLOAD" )
  {
    return (CopyLocalFilesToRemote());
  }
  
  if ( smode == "OCDB" )
  {
    Bool_t ok = Run("LOCAL");
    if (ok)
    {
      ok = MakeOCDBSnapshots();
    }
    return ok;
  }
  
  if ( smode == "TEST" )
  {
    Bool_t ok = Run("LOCAL") && Run("OCDB") && Run("UPLOAD");
    if ( ok )
    {
      ok = (Submit(kTRUE)>0);
    }
    return ok;
  }
  
  if ( smode == "FULL" )
  {
    Bool_t ok = Run("LOCAL")  && Run("OCDB") && Run("UPLOAD");
    if ( ok )
    {
      ok = (Submit(kFALSE)>0);
    }
    return ok;
  }

  if( smode == "SUBMIT" )
  {
    return (Submit(kFALSE)>0);
  }
  
  return kFALSE;
}

//______________________________________________________________________________
void AliMuonAccEffSubmitter::SetPackages(const char* aliroot,
                                         const char* root,
                                         const char* geant3,
                                         const char* api)
{
  /// Set the packages to be used by the jobs
  /// Must be a valid combination, see http://alimonitor.cern.ch/packages/
  ///
  fPackageAliroot = aliroot;
  fPackageRoot = root;
  fPackageGeant3 = geant3;
  fPackageApi = api;
}

//______________________________________________________________________________
TString AliMuonAccEffSubmitter::GetRemoteDir(const char* dir, Bool_t create)
{
  /// Set the target remote directory (on the grid)
  
  if (!RemoteDirectoryExists(dir))
  {
    if (!create)
    {
      AliError(Form("Remote directory %s does not exist", dir));
      return "";
    }
    else
    {
      AliInfo(Form("Remote directory %s does not exist. Trying to create it...",dir));
      if ( !gGrid->Mkdir(dir,"-p") )
      {
        AliError(Form("Could not create remote dir. Sorry."));
        return "";
      }
    }
  }
  return dir;
}

//______________________________________________________________________________
Bool_t AliMuonAccEffSubmitter::SetGenerator(const char* generator)
{
  // set the variable to select the generator macro in Config.C
  
  gSystem->Load("libEVGEN");
    
  fIsValid = kFALSE;
  
  TString generatorFile(Form("%s/%s.C",fTemplateDir.Data(),generator));
  
  Int_t nofMissingVariables(0);
  
  // first check we indeed have such a macro
  if (!gSystem->AccessPathName(generatorFile.Data()))
  {
    TObjArray* variables = GetVariables(generatorFile.Data());
    
    TIter next(variables);
    TObjString* var;
    
    while ( ( var = static_cast<TObjString*>(next())) )
    {
      if ( !fVars->GetValue(var->String()) )
      {
        ++nofMissingVariables;
        AliError(Form("file %s expect the variable %s to be defined, but we've not defined it !",generatorFile.Data(),var->String().Data()));
      }
    }
    
    delete variables;
    
    if ( !nofMissingVariables )
    {
      if (CheckCompilation(generatorFile.Data()))
      {
        fIsValid = kTRUE;
        SetVar("VAR_GENERATOR",Form("%s",generator));        
        TemplateFileList()->Add(new TObjString(Form("%s.C",generator)));
        return kTRUE;
      }
    }
    else
    {
      return kFALSE;
    }
  }
  else
  {
    AliError(Form("Can not work with the macro %s",generatorFile.Data()));
  }
  return kFALSE;
}

//______________________________________________________________________________
Bool_t AliMuonAccEffSubmitter::SetMergedDir(const char* dir, Bool_t create)
{
  // Set the merged directory to be used
  fMergedDir = GetRemoteDir(dir,create);
  return (fMergedDir.Length()>0);
}

//______________________________________________________________________________
Bool_t AliMuonAccEffSubmitter::SetRemoteDir(const char* dir, Bool_t create)
{
  // Set the remote directory to be used
  fRemoteDir = GetRemoteDir(dir,create);
  return (fIsValid = (fRemoteDir.Length()>0));
}


//______________________________________________________________________________
void AliMuonAccEffSubmitter::SetRunList(const char* runList)
{
    // set the runlist from a text file
  if (!fScalers)
  {
    fScalers = new AliAnalysisTriggerScalers(runList,fOCDBPath.Data());
  }
  else
  {
    fScalers->SetRunList(runList);
  }
  UpdateLocalFileList(kTRUE);
}

//______________________________________________________________________________
void AliMuonAccEffSubmitter::SetRunList(int runNumber)
{
  // set the runlist from a text file
  if (!fScalers)
  {
    fScalers = new AliAnalysisTriggerScalers(runNumber,fOCDBPath.Data());
  }
  else
  {
    fScalers->SetRunList(runNumber);      
  }
  UpdateLocalFileList(kTRUE);
}

//______________________________________________________________________________
void AliMuonAccEffSubmitter::SetOCDBPath(const char* ocdbPath)
{
  /// Sets the OCDB path to be used
  
  fOCDBPath = ocdbPath;
  
  if (fScalers)
  {
    // redefine trigger scalers to use the new ocdb path
    AliAnalysisTriggerScalers* ts = new AliAnalysisTriggerScalers(fScalers->GetRunList(),
                                                                  fOCDBPath.Data());
    
    delete fScalers;
    fScalers = ts;
  }
}


//______________________________________________________________________________
void AliMuonAccEffSubmitter::SetOCDBSnapshotDir(const char* dir)
{
  // change the directory used for snapshot
  
  if (gSystem->AccessPathName(Form("%s/OCDB",dir)))
  {
    AliError(Form("Snapshot top directory (%s) should contain an OCDB subdir with runnumbers in there",dir));
  }
  else
  {
    fSnapshotDir = dir;
  }
}

//______________________________________________________________________________
Bool_t AliMuonAccEffSubmitter::SetVar(const char* varname, const char* value)
{
  /// Set a variable
  
  TString s(varname);
  s.ToUpper();
  if (!s.BeginsWith("VAR_"))
  {
    AliError("Variable name should start with VAR_");
    return kFALSE;
  }
  if (!fVars)
  {
    fVars = new TMap;
    fVars->SetOwnerKeyValue(kTRUE,kTRUE);
  }
  
  TObject* o = new TObjString(s);
  fVars->Remove(o);
  
  fVars->Add(o,new TObjString(value));
  
  return kTRUE;
}

//______________________________________________________________________________
Int_t AliMuonAccEffSubmitter::Submit(Bool_t dryRun)
{
  /// Submit multiple production jobs with the format "submit jdl 000run#.xml 000run#".
  ///
  /// Return the number of submitted (master) jobs
  ///
  /// Example:
  /// - outputDir = "/alice/cern.ch/user/p/ppillot/Sim/LHC10h/JPsiPbPb276/AlignRawVtxRaw/ESDs"
  /// - runList must contains the list of run number
  /// - trigger is the (fully qualified) trigger name used to compute the base number of events
  /// - mult is the factor to apply to the number of trigger to get the number of events to be generated
  ///   (# generated events = # triggers x mult
  
  if (!IsValid()) return 0;
  
  AliDebug(1,"");

  gGrid->Cd(RemoteDir());
  
  if (!RemoteFileExists(RunJDLName()))
  {
    AliError(Form("file %s does not exist in %s", RunJDLName().Data(), RemoteDir().Data()));
    return 0;
  }
  
  if ( !fScalers )
  {
    AliError("No run list set. Use SetRunList");
    return 0;
  }
  const std::vector<int>& runs = fScalers->GetRunList();
  
  if (runs.empty())
  {
    AliError("No run to work with");
    return 0;
  }
  
  //  cout << "total number of selected MB events = " << totEvt << endl;
  //  cout << "required number of generated events = " << nGenEvents << endl;
  //  cout << "number of generated events per MB event = " << ratio << endl;
  //  cout << endl;
  
  std::cout << "run\tchunks\tevents" << std::endl;
  std::cout << "----------------------" << std::endl;
  
  Int_t nJobs(0);
  Int_t nEvts(0);
  
  for (std::vector<int>::size_type i=0; i < runs.size(); ++i)
  {
    Int_t runNumber = runs[i];
    
    Int_t nEvtRun(fFixedNofEvents);
    
    if ( fRatio > 0 )
    {
      AliAnalysisTriggerScalerItem* trigger = fScalers->GetTriggerScaler(runNumber, "L2A", ReferenceTrigger().Data());
    
      if (!trigger)
      {
        AliError(Form("Could not get trigger %s for run %09d",ReferenceTrigger().Data(),runNumber));
        continue;
      }
      nEvtRun = TMath::Nint(fRatio * trigger->Value());
    }
    
    Int_t nChunk = 1;
    
    while (nEvtRun/nChunk+0.5 > MaxEventsPerChunk())
    {
      ++nChunk;
    }
    
    Int_t nEvtChunk = TMath::Nint(nEvtRun/nChunk + 0.5);
    
    nJobs += nChunk;
    
    nEvts += nChunk*nEvtChunk;
    
    std::cout << runNumber << "\t" << nChunk << "\t" << nEvtChunk << std::endl;
    
    TString query(Form("submit %s %d %d %d", RunJDLName().Data(), runNumber, nChunk, nEvtChunk));
    
    std::cout << query.Data() << " ..." << std::flush;
    
    TGridResult* res = 0x0;
    
    if (!dryRun)
    {
      res = gGrid->Command(query);
    }
    
    if (res)
    {
      TString cjobId1 = res->GetKey(0,"jobId");
      
      if (!cjobId1.Length())
      {
        std::cout << " FAILED" << std::endl << std::endl;
        gGrid->Stdout();
        gGrid->Stderr();
      }
      else
      {
        std::cout << "DONE" << std::endl;
        std::cout << Form("   --> the job Id is: %s",cjobId1.Data()) << std::endl << std::endl;
      }
    }
    else
    {
      std::cout << " FAILED" << std::endl << std::endl;
    }
    
    delete res;
  }
  
  std::cout << std::endl
  << "total number of jobs = " << nJobs << std::endl
  << "total number of generated events = " << nEvts << std::endl
  << std::endl;
  
  return nJobs;
}

//______________________________________________________________________________
TObjArray* AliMuonAccEffSubmitter::TemplateFileList() const
{
  /// Return (after createing and filling it if needed)
  /// the internal file list with paths from the template directory
  
  if (!fTemplateFileList)
  {
    fTemplateFileList = new TObjArray;
    fTemplateFileList->SetOwner(kTRUE);
    
    fTemplateFileList->Add(new TObjString("CheckESD.C"));
    fTemplateFileList->Add(new TObjString("CheckAOD.C"));
    fTemplateFileList->Add(new TObjString("AODtrain.C"));
    fTemplateFileList->Add(new TObjString("validation.sh"));
    if ( fExternalConfig.Length() > 0 )
    {
      fTemplateFileList->Add(new TObjString(fExternalConfig));
    }
    else
    {
      fTemplateFileList->Add(new TObjString("Config.C"));
    }
    fTemplateFileList->Add(new TObjString("rec.C"));
    fTemplateFileList->Add(new TObjString("sim.C"));
    fTemplateFileList->Add(new TObjString("simrun.C"));
    fTemplateFileList->Add(new TObjString(RunJDLName().Data()));
    if ( fUseAODMerging )
    {
      fTemplateFileList->Add(new TObjString(MergeJDLName(kFALSE).Data()));
      fTemplateFileList->Add(new TObjString(MergeJDLName(kTRUE).Data()));
      fTemplateFileList->Add(new TObjString("AOD_merge.sh"));
      fTemplateFileList->Add(new TObjString("validation_merge.sh"));
    }
  }
  
  return fTemplateFileList;
}

//______________________________________________________________________________
void AliMuonAccEffSubmitter::UpdateLocalFileList(Bool_t clearSnapshots)
{
  /// Update the list of local files
  
  if (!fScalers) return;
  
  if ( clearSnapshots )
  {
    TIter next(LocalFileList());
    TObjString* file;
    
    while ( ( file = static_cast<TObjString*>(next())) )
    {
      if ( file->String().Contains("OCDB_") )
      {
        LocalFileList()->Remove(file);
      }
    }
    LocalFileList()->Compress();
  }

  const std::vector<int>& runs = fScalers->GetRunList();
  
  const char* type[] = { "sim","rec" };
  
  for ( std::vector<int>::size_type i = 0; i < runs.size(); ++i )
  {
    Int_t runNumber = runs[i];
    
    for ( Int_t t = 0; t < 2; ++t )
    {
      TString snapshot(Form("%s/OCDB/%d/OCDB_%s.root",SnapshotDir().Data(),runNumber,type[t]));
      
      if ( !gSystem->AccessPathName(snapshot.Data()) )
      {
        if ( !LocalFileList()->FindObject(snapshot.Data()) )
        {
          LocalFileList()->Add(new TObjString(snapshot));
        }
      }
    }
  }
}

//______________________________________________________________________________
void AliMuonAccEffSubmitter::UseOCDBSnapshots(Bool_t flag)
{
  /// Whether or not to use OCDB snapshots
  /// Using OCDB snapshots will speed-up both the sim and reco initialization
  /// phases on each worker node, but takes time to produce...
  /// So using them is not always a win-win...
  
  fUseOCDBSnapshots = flag;
  if ( flag )
  {
    SetVar("VAR_OCDB_SNAPSHOT","kTRUE");
  }
  else
  {
    SetVar("VAR_OCDB_SNAPSHOT","kFALSE");
  }
  
  UpdateLocalFileList();
}

//______________________________________________________________________________
void AliMuonAccEffSubmitter::UseAODMerging(Bool_t flag)
{
  /// whether or not we should generate JDL for merging AODs
  
  fUseAODMerging = flag;
  // FIXME: here should update the TemplateFileList() (and LocalFileList as well ?)
}
