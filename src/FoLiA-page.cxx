#include <unistd.h> // getopt, unlink
#include <sys/stat.h>
#include <sys/types.h>
#include <string>
#include <list>
#include <map>
#include <vector>
#include <stdexcept>
#include <iostream>
#include <fstream>
#include "libfolia/document.h"
#include "ticcutils/XMLtools.h"
#include "ticcutils/StringOps.h"
#include "ticcutils/zipper.h"
#include "ticcutils/FileUtils.h"
#include "config.h"
#ifdef HAVE_OPENMP
#include "omp.h"
#endif

using namespace	std;

bool verbose = false;
bool predict = false;

enum zipType { NORMAL, GZ, BZ2, UNKNOWN };

xmlNode *getNode( xmlNode *pnt, const string& tag ){
  while ( pnt ){
    if ( pnt->type == XML_ELEMENT_NODE && TiCC::Name(pnt) == tag ){
      return pnt;
    }
    else {
      xmlNode *res  = getNode( pnt->children, tag );
      if ( res )
	return res;
    }
    pnt = pnt->next;
  }
  return 0;
}

void getNodes( xmlNode *pnt, const string& tag, vector<xmlNode*>& res ){
  while ( pnt ){
    if ( pnt->type == XML_ELEMENT_NODE && TiCC::Name(pnt) == tag ){
      res.push_back( pnt );
    }
    getNodes( pnt->children, tag, res );
    pnt = pnt->next;
  }
}

vector<xmlNode*> getNodes( xmlNode *pnt, const string& tag ){
  vector<xmlNode*> res;
  getNodes( pnt, tag, res );
  return res;
}

xmlDoc *getXml( const string& file, zipType& type ){
  type = UNKNOWN;
  if ( TiCC::match_back( file, ".xml" ) ){
    type = NORMAL;
  }
  else if ( TiCC::match_back( file, ".xml.gz" ) ){
    type = GZ;
  }
  else if ( TiCC::match_back( file, ".xml.bz2" ) ){
    type = BZ2;
  }
  else {
    cerr << "problem detecting type of file: " << file << endl;
    return 0;
  }
  if ( type == NORMAL ){
    return xmlReadFile( file.c_str(), 0, XML_PARSE_NOBLANKS );
  }
  string buffer;
  if ( type == GZ ){
    buffer = TiCC::gzReadFile( file );
  }
  else if ( type == BZ2 ){
    buffer = TiCC::bz2ReadFile( file );
  }
  return xmlReadMemory( buffer.c_str(), buffer.length(),
			0, 0, XML_PARSE_NOBLANKS );
}

string extractContent( xmlNode* pnt ) {
  string result;
  if ( pnt ){
    result = TiCC::XmlContent(pnt);
    if ( result == "" )
      return extractContent( pnt->children );
  }
  return result;
}


void appendStr( folia::FoliaElement *par, int& pos,
		const string& val, const string& id,
		const string& file ){
  if ( !val.empty() ){
    folia::String *str = new folia::String( par->doc(),
					    "id='" + par->id()
					    + "." + id + "'" );
    par->append( str );
    str->settext( val, pos, "OCR" );
    pos += val.length();
    folia::Alignment *h = new folia::Alignment( "href='" + file + "'" );
    str->append( h );
    folia::AlignReference *a =
      new folia::AlignReference( "id='" + id + "', type='str'" );
    h->append( a );
  }
}

void process( folia::FoliaElement *out,
	      const vector<string>& vec,
	      const vector<string>& refs,
	      const string& file ){
  for ( size_t i=0; i < vec.size(); ++i ){
    vector<string> parts;
    TiCC::split( vec[i], parts );
    string parTxt;
    for ( size_t j=0; j< parts.size(); ++j ){
      parTxt += parts[j];
      if ( j != parts.size()-1 )
	parTxt += " ";
    }
    folia::Paragraph *par
      = new folia::Paragraph( out->doc(),
			      "id='" + out->id() + "." + refs[i] + "'");
    par->settext( parTxt, "OCR" );
    out->append( par );
    int pos = 0;
    for ( size_t j=0; j< parts.size(); ++j ){
      string id = "word_" + TiCC::toString(j);
      appendStr( par, pos, parts[j], id, file );
    }
  }
}

string getOrg( xmlNode *node ){
  string result;
  if ( node->type == XML_CDATA_SECTION_NODE ){
    string cdata = (char*)node->content;
    string::size_type pos = cdata.find("Original.Path");
    if ( pos != string::npos ){
      string::size_type epos = cdata.find( "/meta", pos );
      string longName = cdata.substr( pos+15, epos - pos - 16 );
      pos = longName.rfind( "/" );
      result = longName.substr( pos+1 );
    }
  }
  return result;
}

void convert_pagexml( const string& fileName,
		      const string& outputDir,
		      const zipType outputType ){
  if ( verbose ){
#pragma omp critical
    {
      cout << "start handling " << fileName << endl;
    }
  }
  zipType inputType;
  xmlDoc *xdoc = getXml( fileName, inputType );
  xmlNode *root = xmlDocGetRootElement( xdoc );
  xmlNode* comment = getNode( root, "Comment" );
  string orgFile;
  if ( comment ){
    orgFile = getOrg( comment->children );
  }
  if ( orgFile.empty() ) {
#pragma omp critical
    {
      cerr << "unable to retrieve an original filename from " << fileName << endl;
    }
    return;
  }

  vector<xmlNode*> order = getNodes( root, "ReadingOrder" );
  if ( order.size() != 1 ){
#pragma omp critical
    {
      cerr << "Problem finding 1 ReadingOrder node in " << fileName << endl;
    }
    return;
  }
  order = getNodes( order[0], "RegionRefIndexed" );
  string title;
  map<string,int> refs;
  vector<string> backrefs( order.size() );;
  for ( size_t i=0; i < order.size(); ++i ){
    string ref = TiCC::getAttribute( order[i], "regionRef" );
    string index = TiCC::getAttribute( order[i], "index" );
    int id = TiCC::stringTo<int>( index );
    refs[ref] = id;
    backrefs[id] = ref;
  }

  vector<string> regionStrings( refs.size() );
  vector<xmlNode*> regions = getNodes( root, "TextRegion" );
  for ( size_t i=0; i < regions.size(); ++i ){
    string index = TiCC::getAttribute( regions[i], "id" );
    map<string,int>::const_iterator it = refs.find(index);
    if ( it == refs.end() ){
      if ( verbose ){
#pragma omp critical
	{
	  cerr << "ignoring Unordered id " << index << " in " << fileName << endl;
	}
      }
    }
    else {
      xmlNode *unicode = getNode( regions[i], "Unicode" );
      regionStrings[it->second] = TiCC::XmlContent( unicode );
    }
  }
  xmlFreeDoc( xdoc );
  // for ( size_t i=0; i < regionStrings.size(); ++i ){
  //   cerr << "[" << i << "]-" << regionStrings[i] << endl;
  // }

  string docid = orgFile;
  folia::Document doc( "id='" + docid + "'" );
  doc.declare( folia::AnnotationType::STRING, "OCR",
	       "annotator='folia-hocr', datetime='now()'" );
  folia::Text *text = new folia::Text( "id='" + docid + ".text'" );
  doc.append( text );
  process( text, regionStrings, backrefs, docid );

  string outName = outputDir;
  outName += docid + ".folia.xml";
  zipType type = inputType;
  if ( outputType != NORMAL )
    type = outputType;
  if ( type == BZ2 )
    outName += ".bz2";
  else if ( type == GZ )
    outName += ".gz";
  vector<folia::Paragraph*> pv = doc.paragraphs();
  if ( pv.size() == 0 ||
       ( pv.size() == 1 && pv[0]->size() == 0 ) ){
    // no paragraphs, or just 1 without data
#pragma omp critical
    {
      cerr << "skipped empty result : " << outName << endl;
    }
  }
  else {
    doc.save( outName );
    if ( verbose ){
#pragma omp critical
      {
	cout << "created " << outName << endl;
      }
    }
  }
}

int main( int argc, char *argv[] ){
  if ( argc < 2	){
    cerr << "Usage: [-t number_of_threads] [-o outputdir] dir/filename " << endl;
    exit(EXIT_FAILURE);
  }
  int opt;
  int numThreads=1;
  string outputDir;
  zipType outputType = NORMAL;
  while ((opt = getopt(argc, argv, "bcght:vVo:p")) != -1) {
    switch (opt) {
    case 'b':
      outputType = BZ2;
      break;
    case 'g':
      outputType = GZ;
      break;
    case 't':
      numThreads = atoi(optarg);
      break;
    case 'v':
      verbose = true;
      break;
    case 'p':
      predict = true;
      break;
    case 'V':
      cerr << PACKAGE_STRING << endl;
      exit(EXIT_SUCCESS);
      break;
    case 'h':
      cerr << "Usage: FoLiA-page [options] file/dir" << endl;
      cerr << "\t-t\t number_of_threads" << endl;
      cerr << "\t-h\t this messages " << endl;
      cerr << "\t-o\t output directory " << endl;
      cerr << "\t-b\t create bzip2 files (.bz2)" << endl;
      cerr << "\t-g\t create gzip files (.gz)" << endl;
      cerr << "\t-v\t verbose output " << endl;
      cerr << "\t-V\t show version " << endl;
      exit(EXIT_SUCCESS);
      break;
    case 'o':
      outputDir = string(optarg) + "/";
      break;
    default: /* '?' */
      cerr << "Usage: alto [-t number_of_threads] [-o output_dir] dir/filename " << endl;
      exit(EXIT_FAILURE);
    }
  }
  vector<string> fileNames;
  string dirName;
  if ( !outputDir.empty() ){
    string name = outputDir;
    if ( !TiCC::isDir(name) ){
      int res = mkdir( name.c_str(), S_IRWXU|S_IRWXG );
      if ( res < 0 ){
	cerr << "outputdir '" << name
	     << "' doesn't existing and can't be created" << endl;
	exit(EXIT_FAILURE);
      }
    }
  }
  if ( !argv[optind] ){
    exit(EXIT_FAILURE);
  }
  string name = argv[optind];
  if ( !( TiCC::isFile(name) || TiCC::isDir(name) ) ){
    cerr << "parameter '" << name << "' doesn't seem to be a file or directory"
	 << endl;
    exit(EXIT_FAILURE);
  }
  if ( TiCC::isFile(name) ){
    if ( TiCC::match_back( name, ".tar" ) ){
      cerr << "TAR files are not supported yet." << endl;
      exit(EXIT_FAILURE);
    }
    else {
      fileNames.push_back( name );
      string::size_type pos = name.rfind( "/" );
      if ( pos != string::npos )
	dirName = name.substr(0,pos);
    }
  }
  else {
    fileNames = TiCC::searchFilesMatch( name, "*.xml" );
  }
  size_t toDo = fileNames.size();
  if ( toDo == 0 ){
    cerr << "no matching files found." << endl;
    exit(EXIT_FAILURE);
  }
  if ( toDo > 1 )
    cout << "start processing of " << toDo << " files " << endl;
  if ( numThreads >= 1 ){
    omp_set_num_threads( numThreads );
  }

#pragma omp parallel for shared(fileNames)
  for ( size_t fn=0; fn < fileNames.size(); ++fn ){
    convert_pagexml( fileNames[fn], outputDir, outputType );
  }
  cout << "done" << endl;
  exit(EXIT_SUCCESS);
}
