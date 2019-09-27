/**
\file  GreedyRegistration.cxx

\brief GreedyRegistration comand line interface.

Dependecies: ITK (module_review, module_skullstrip enabled)

https://www.cbica.upenn.edu/sbia/software/ <br>
software@cbica.upenn.edu

Copyright (c) 2018 University of Pennsylvania. All rights reserved. <br>
See COPYING file or https://www.cbica.upenn.edu/sbia/software/license.html

*/

#include "itkImageFileReader.h"
#include "itkImageFileWriter.h"
//#include "GreedyRegistration.h"
#include "itkDOMNodeXMLReader.h"
#include "itkDOMNodeXMLWriter.h"
#include "itkDOMNode.h"
#include "CommandLineHelper.h"

#include <iostream>
#include <sstream>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>
#include <cerrno>
#include <chrono>

#include "lddmm_common.h"
#include "lddmm_data.h"
#include "GreedyAPI.h"

#include <itkImageFileReader.h>
#include <itkAffineTransform.h>
#include <itkTransformFactory.h>
#include <itkTimeProbe.h>

#include "MultiImageRegistrationHelper.h"
#include "FastWarpCompositeImageFilter.h"
#include <vnl/vnl_cost_function.h>
#include <vnl/vnl_random.h>
#include <vnl/algo/vnl_powell.h>
#include <vnl/algo/vnl_svd.h>
#include <vnl/vnl_trace.h>

#include "cbicaITKImageInfo.h"
#include "cbicaITKUtilities.h"
#include "cbicaCmdParser.h"
#include "cbicaLogging.h"
#include "cbicaITKSafeImageIO.h"
#include "cbicaUtilities.h"


std::string inputImageFile, outputImageFile, targetImageFile, matrix, fixedImage, inputFileString, outputFileString;
std::string metrics = "NMI", g_iterations = "100x50x5", nccradii = "5x5x5";
std::vector<std::string> inputImageFiles, outputImageFiles, matrixImageFiles;


template <unsigned int VDim, typename TReal>
class GreedyRunner
{
public:
  static int Run(GreedyParameters &param, CommandLineHelper &clHelper)
  {
    // we go with the normal registration for non 4D images
    auto movingImageInfo = cbica::ImageInfo(inputImageFiles[0]);
    if (movingImageInfo.GetImageDimensions() != 4)
    {
      GreedyApproach<VDim, TReal> greedy;
      return greedy.Run(param);
    }
    else
    {
      /// this approach will take a *very* round-about way to work
      /// instead, we should add Greedy as a submodule and then use
      /// the greedy executable to do all the things that are needed

      if (param.mode == GreedyParameters::RESLICE)
      {
        std::cerr << "This condition should not come up at all.\n";
        exit(EXIT_FAILURE);
      }
      // conditions to work on:
      // movingImage = 4D && fixedImage == 4D
      // movingImage = 4D && fixedImage == 3D
      using TMovingImageType = itk::Image< TReal, 4 >;
      using TMovingExtractedImageType = itk::Image< TReal, 3 >;

      auto temporaryDataDir = cbica::createTemporaryDirectory();

      // this is the image we will use to register everything
      auto fixedImageForRegistering_file = cbica::normPath(temporaryDataDir + "/fixedImage.nii.gz");
      
      if (VDim == 4)
      {
        std::cout << "A 4D moving image was detected; the first in the series will be used as fixed image template.\n";
        auto tempFixedExtracted = 
          cbica::GetExtractedImages< TMovingImageType, TMovingExtractedImageType >(
          cbica::ReadImage< TMovingImageType >(fixedImage)
          );

        cbica::WriteImage< TMovingExtractedImageType >(tempFixedExtracted[0], fixedImageForRegistering_file);
      }
      else
      {
        std::cout << ":::[DEBUG] Checking which one works quicker/better: the best one should be chosen and this code pruned.\n";
        std::cout << ":::======= Using the cbica::copyFile function took ";
        auto copy_start = std::chrono::steady_clock::now();
        if (!cbica::copyFile(fixedImage, fixedImageForRegistering_file))
        {
          std::cerr << "Something went wrong with copying the fixedImage.\n";
          exit(EXIT_FAILURE);
        }
        auto copy_end = std::chrono::steady_clock::now();
        std::cout << 
          float(
            std::chrono::duration_cast<std::chrono::milliseconds>(copy_end - copy_start).count()
            ) / 1000.0 << " seconds.\n";

        std::cout << ":::======= Using ITK Read -> Write function took ";
        auto io_start = std::chrono::steady_clock::now();
        cbica::WriteImage< TMovingExtractedImageType >(
          cbica::ReadImage< TMovingExtractedImageType >(fixedImage), fixedImageForRegistering_file);
        auto io_end = std::chrono::steady_clock::now();
        std::cout <<
          float(
            std::chrono::duration_cast<std::chrono::milliseconds>(io_end - io_start).count()
            ) / 1000.0 << " seconds.\n";
      }

      // looping through all the moving images
      for (size_t totalMovingImages = 0; totalMovingImages < inputImageFiles.size(); totalMovingImages++)
      {
        // GreedyRegistration -reg -trf -i moving.nii.gz -f fixed.nii.gz 
        // -o output.nii.gz -t matrix.mat -a -m MI -n 100x50x5",

        // move all extracted images to a single structure that keeps getting overwritten to conserve memory
        auto movingImagePointers_extracted = 
          cbica::GetExtractedImages< TMovingImageType, TMovingExtractedImageType >(
            cbica::ReadImage< TMovingImageType >(inputImageFiles[totalMovingImages])
            );

        std::vector< typename TMovingExtractedImageType::Pointer > movingImagePointers_extracted_registered;
        movingImagePointers_extracted_registered.resize(movingImagePointers_extracted.size());

        std::vector< std::string > movingImageExtracted_files, movingImageExtracted_output_files;
        movingImageExtracted_files.resize(movingImagePointers_extracted.size());
        movingImageExtracted_output_files.resize(movingImagePointers_extracted.size());

        for (size_t extractedImages = 0;
          extractedImages < movingImagePointers_extracted.size();
          extractedImages++)
        {
          movingImageExtracted_files[extractedImages] =
            cbica::normPath(temporaryDataDir + "/movingImageExtracted_" + 
              std::to_string(extractedImages) + ".nii.gz");

          movingImageExtracted_output_files[extractedImages] =
            cbica::normPath(temporaryDataDir + "/movingImageExtractedOutput_" +
              std::to_string(extractedImages) + ".nii.gz");

          // write out the all the images in the series
          cbica::WriteImage< TMovingExtractedImageType >(
            movingImagePointers_extracted[extractedImages], 
            movingImageExtracted_files[extractedImages]);
        }

        // the assumption here is that in a single 4D series, the images inside will be co-registered
        param.output = matrixImageFiles[totalMovingImages];

        param.inputs.clear(); // remove whatever was done previously
        ImagePairSpec ip;
        ip.weight = 1.0; // this is always hard-coded in any case
        ip.fixed = fixedImageForRegistering_file;
        ip.moving = movingImageExtracted_files[0]; // something to write 
        param.inputs.push_back(ip);

        GreedyApproach< 3, TReal > greedy; // the registration is always run on 3D images at this point
        return greedy.Run(param);

        // at this point, the transformation matrix is already present and we just need to apply it

        param.reslice_param.ref_image = fixedImageForRegistering_file;
        param.mode = GreedyParameters::RESLICE;

        TransformSpec spec;
        ResliceSpec reslice;
        InterpSpec interp_current;

        param.reslice_param.transforms.clear();
        param.reslice_param.images.clear();

        for (size_t extractedImages = 0; 
          extractedImages < movingImagePointers_extracted.size(); 
          extractedImages++)
        {
          reslice.interp = interp_current;
          reslice.moving = movingImageExtracted_files[extractedImages];
          reslice.output = movingImageExtracted_output_files[extractedImages];

          param.reslice_param.images.push_back(reslice);

          if (!cbica::fileExists(matrixImageFiles[totalMovingImages]))
          {
            std::cerr << "Something went wrong while registering the first image; the registration will stop.\n";
            exit(EXIT_FAILURE);
          }
          spec = clHelper.read_transform_spec(matrixImageFiles[totalMovingImages]);

          param.reslice_param.transforms.push_back(spec);

          GreedyApproach< 3, TReal > greedy_reslicer; // here, we do the reslicing
          return greedy_reslicer.Run(param);
          
          if (cbica::fileExists(movingImageExtracted_output_files[extractedImages]))
          {
            // now, put the registered images in the vector to combine in the next step
            movingImagePointers_extracted_registered[extractedImages] =
              cbica::ReadImage< TMovingExtractedImageType >(
                movingImageExtracted_output_files[extractedImages]);
          }
        }

        // now, let's join these guys together and write the combined thing out
        auto joinedImage = cbica::GetJoinedImage< TMovingExtractedImageType, TMovingImageType >(
          movingImagePointers_extracted_registered
          );
        cbica::WriteImage< TMovingImageType >(joinedImage, outputImageFiles[totalMovingImages]);
      }

      cbica::deleteDir(temporaryDataDir); // ensure all temporary data gets deleted
      std::cout << "Finished registration.\n";
      exit(EXIT_SUCCESS);
    }
  }
};

int main(int argc, char** argv)
{
  cbica::CmdParser parser(argc, argv, "GreedyRegistration");

  parser.addRequiredParameter("i", "movingImage", cbica::Parameter::FILE, "NIfTI", "Input Image for processing", "Becomes moving image in Registration mode");
  parser.addRequiredParameter("f", "fixedImage", cbica::Parameter::STRING, "NIfTI", "Fixed Image for registration");
  parser.addRequiredParameter("o", "outputImage", cbica::Parameter::FILE, "NIfTI", "Output Image for processing");
  parser.addRequiredParameter("t", "matrix", cbica::Parameter::STRING, "N.A", "Registration Matrix");

  parser.addOptionalParameter("reg", "registration", cbica::Parameter::NONE, "N.A", "Switch to registration mode");
  parser.addOptionalParameter("trf", "transformation", cbica::Parameter::NONE, "N.A", "Switch to transformation mode");

  parser.addOptionalParameter("a", "affine", cbica::Parameter::NONE, "N.A", "Affine Registration(Default)");
  parser.addOptionalParameter("r", "rigid", cbica::Parameter::NONE, "N.A", "Rigid Registration");
  parser.addOptionalParameter("m", "metrics", cbica::Parameter::STRING, "none", "MI: mutual information", "NMI(Default): normalized mutual information", "NCC -r 2x2x2: normalized cross-correlation");
  parser.addOptionalParameter("ri", "radius", cbica::Parameter::STRING, "none", "Patch radius for metrics", "Eg: 2x2x2");

  parser.addOptionalParameter("n", "greedyIterations", cbica::Parameter::STRING, "none", "Number of iterations per level of multi-res (Default: 100x50x5)", "Corresponds to low level, Mid Level and High Level resolution", "Pattern: NxNxN");
  parser.addOptionalParameter("th", "threads", cbica::Parameter::INTEGER, "none", "Number of threads for algorithm", "If not suppllied gets set to default 4");
  //parser.exampleUsage("-reg -trf -i moving.nii.gz -f fixed.nii.gz -o output.nii.gz -t matrix.mat -a -m MI -n 100x50x5 -th 4");

  parser.addApplicationDescription("This does affine registration based on Greedy");
  parser.addExampleUsage("-reg -trf -i moving.nii.gz -f fixed.nii.gz -o output.nii.gz -t matrix.mat -a -m MI -n 100x50x5",
    "This registers the moving image 'moving.nii.gz' with fixed image 'fixed.nii.gz.' with output at 'output.nii.gz'");

  
  CommandLineHelper cl(argc, argv);

  if (parser.isPresent("i"))
  {
    parser.getParameterValue("i", inputFileString);
    //parser.getParameterValue("i", inputImageFile);
    inputImageFiles = cbica::stringSplit(inputFileString, ",");
  }

  if (parser.isPresent("f"))
  {
    parser.getParameterValue("f", fixedImage);
  }

  if (parser.isPresent("o"))
  {
    std::string outputFileString;
    parser.getParameterValue("o", outputFileString);
    //parser.getParameterValue("o", outputImageFile);
    outputImageFiles = cbica::stringSplit(outputFileString, ",");
  }

  if (parser.isPresent("t"))
  {
    std::string matrixFileString;
    parser.getParameterValue("t", matrixFileString);
    matrixImageFiles = cbica::stringSplit(matrixFileString, ",");
  }

  /*-------------Greedy Registration------------*/
  GreedyParameters param;
  GreedyParameters::SetToDefaults(param);

  if (parser.isPresent("th"))
  {
    int threads;
    parser.getParameterValue("th", threads);
    param.threads = threads;
  }

  if (inputImageFiles.size() != outputImageFiles.size()
    || inputImageFiles.size() != matrixImageFiles.size() || outputImageFiles.size() != matrixImageFiles.size())
  {
    std::cerr << "--> Error: Number of moving images does not match with output images and matrices" << std::endl;
    return EXIT_FAILURE;
  }

  std::string tempFolderLocation = cbica::normPath(cbica::getUserHomeDirectory() + "/.CaPTk");
  cbica::createDir(tempFolderLocation);
  if (cbica::IsDicom(fixedImage))
  {
    // dicom image detected
    cbica::WriteImage< ImageTypeFloat3D >(
      cbica::ReadImage< ImageTypeFloat3D >(fixedImage),
      tempFolderLocation + "/tempDicomConverted_fixed.nii.gz"
      );
    fixedImage = tempFolderLocation + "/tempDicomConverted_fixed.nii.gz";
  }

  auto movingImageInfo_first = cbica::ImageInfo(inputImageFiles[0]);

  for (int i = 0; i < inputImageFiles.size(); i++)
  {

    if (parser.isPresent("reg")) {
      std::cout << "--> Analyzing parameters:" << std::endl;

      InterpSpec interp_current;
      bool affineMode = false;
      float current_weight = 1.0;

      ImagePairSpec ip;
      ip.weight = current_weight;
      ip.fixed = fixedImage;
      if (cbica::IsDicom(inputImageFiles[i]))
      {
        // dicom image detected
        cbica::WriteImage< ImageTypeFloat3D >(
          cbica::ReadImage< ImageTypeFloat3D >(inputImageFiles[i]),
          tempFolderLocation + "/tempDicomConverted_moving.nii.gz"
          );
        ip.moving = tempFolderLocation + "/tempDicomConverted_moving.nii.gz";
      }
      else
      {
        ip.moving = inputImageFiles[i];
      }
      param.inputs.push_back(ip);

      if (parser.isPresent("a")) {
        std::cout << "--> Registration Mode: Affine " << std::endl;

        param.mode = GreedyParameters::AFFINE;
        param.affine_dof = GreedyParameters::DOF_AFFINE;
      }

      if (parser.isPresent("r")) {
        std::cout << "--> Registration Mode: Rigid " << std::endl;

        param.mode = GreedyParameters::AFFINE;
        param.affine_dof = GreedyParameters::DOF_RIGID;
      }

      if (parser.isPresent("n")) {
        parser.getParameterValue("n", g_iterations);
        std::cout << "--> Number of iterations: " << g_iterations << std::endl;
        param.iter_per_level = cl.read_int_vector(g_iterations);
      }
      else {
        std::cout << "--> Using default iterations: 100x100" << std::endl;
      }

      if (parser.isPresent("m"))
      {
        parser.getParameterValue("m", metrics);

        if (metrics == "NCC" || metrics == "ncc")
        {
          metrics = "NCC";
          param.metric = GreedyParameters::NCC;
        }
        else if (metrics == "MI" || metrics == "mi")
        {
          metrics = "MI";
          param.metric = GreedyParameters::MI;
        }
        else if (metrics == "NMI" || metrics == "nmi")
        {
          metrics = "NMI";
          param.metric = GreedyParameters::NMI;
        }
        else if (metrics == "ssd" || metrics == "SSD")
        {
          metrics = "SSD";
          param.metric = GreedyParameters::SSD;
        }

        std::cout << "--> Metric used: " << metrics << std::endl;

        if (parser.isPresent("ri")) {
          std::string val;
          parser.getParameterValue("ri", val);
          std::cout << "--> Patch radius used for metrics: " << val << std::endl;
          param.metric_radius = cl.read_int_vector(val);
        }
      }

      std::cout << "--> Transformation matrix output file name: " << matrixImageFiles[i] << std::endl;
      param.output = matrixImageFiles[i];

      std::cout << "--> Looking for fixed image: " << std::endl;

      if (!cbica::fileExists(fixedImage))
      {
        std::cerr << "--> Fixed image file not found :'" << fixedImage << "'\n";
        return EXIT_FAILURE;
      }
      std::cout << "--> Fixed image found: " + fixedImage << std::endl;

      std::cout << "--> Looking for moving image: " << std::endl;

      if (!cbica::fileExists(inputImageFiles[i]))
      {
        std::cerr << "--> Moving image file not found :'" << inputImageFiles[i] << "'\n";
        return EXIT_FAILURE;
      }
      std::cout << "--> Moving image found: " + inputImageFiles[i] << std::endl;

      if (param.threads > 0)
      {
        std::cout << "--> Limiting the number of threads to " << param.threads << std::endl;
        itk::MultiThreader::SetGlobalMaximumNumberOfThreads(param.threads);
      }
      else
      {
        std::cout << "--> Executing with the default number of threads: " << itk::MultiThreader::GetGlobalDefaultNumberOfThreads() << std::endl;

      }

      // Some parameters may be specified as either vector or scalar, and need to be verified
      if (param.epsilon_per_level.size() != param.iter_per_level.size())
      {
        if (param.epsilon_per_level.size() == 1)
        {
          param.epsilon_per_level =
            std::vector<double>(param.iter_per_level.size(), param.epsilon_per_level.back());
        }
        else
        {
          throw GreedyException("--> Mismatch in size of vectors supplied with -n and -e options");
        }
      }

      auto fixedImageInfo = cbica::ImageInfo(fixedImage);
      auto movingImageInfo = cbica::ImageInfo(inputImageFiles[i]);

      // do not perform dimension sanity check for 4D moving image
      if (movingImageInfo.GetImageDimensions() != 4)
      {
        if (fixedImageInfo.GetImageDimensions() != movingImageInfo.GetImageDimensions())
        {
          std::cerr << "--> Image dimensions between the Moving and Fixed image do not match.\n";
          return EXIT_FAILURE;
        }
      }
      else
      {
        if (movingImageInfo_first.GetImageDimensions() != movingImageInfo.GetImageDimensions())
        {
          std::cerr << "--> Image dimensions between the different Movin Images do not match.\n";
          return EXIT_FAILURE;
        }
      }

      param.dim = fixedImageInfo.GetImageDimensions();

      if (param.flag_float_math)
      {
        switch (fixedImageInfo.GetImageDimensions())
        {
        case 2: GreedyRunner<2, float>::Run(param, cl); break;
        case 3: GreedyRunner<3, float>::Run(param, cl); break;
        case 4: GreedyRunner<4, float>::Run(param, cl); break;
        default: throw GreedyException("--> Wrong number of dimensions requested: %d", param.dim);
        }
      }
      else
      {
        switch (fixedImageInfo.GetImageDimensions())
        {
        case 2: GreedyRunner<2, double>::Run(param, cl); break;
        case 3: GreedyRunner<3, double>::Run(param, cl); break;
        case 4: GreedyRunner<4, double>::Run(param, cl); break;
        default: throw GreedyException("--> Wrong number of dimensions requested: %d", param.dim);
        }
      }

      std::cout << "--> Finished registration.\n";

      //GreedyParameters param;
      //GreedyParameters::SetToDefaults(param);

      if (parser.isPresent("trf")) 
      {
        std::cout << "--> Starting transformation.\n";


        if (parser.isPresent("f")) 
        {
          parser.getParameterValue("f", fixedImage);
        }

        param.reslice_param.ref_image = fixedImage;
        TransformSpec spec;
        ResliceSpec reslice;

        InterpSpec interp_current;
        reslice.interp = interp_current;
        reslice.moving = inputImageFiles[i];
        reslice.output = outputImageFiles[i];

        param.reslice_param.images.push_back(reslice);

        param.mode = GreedyParameters::RESLICE;

        std::cout << "--> Looking for transformation matrix: " + matrixImageFiles[i] << std::endl;

        if (!cbica::fileExists(matrixImageFiles[i])) {
          std::cerr << "--> Transformation matrix not found at: " << matrixImageFiles[i] << "'\n";
          return EXIT_FAILURE;
        }
        std::cout << "--> Transformation Matrix found: " + matrixImageFiles[i] << std::endl;
        spec = cl.read_transform_spec(matrixImageFiles[i]);

        param.reslice_param.transforms.push_back(spec);


        /*if (param.threads > 0)
        {
            std::cout << "--> Limiting the number of threads to " << param.threads << std::endl;
            itk::MultiThreader::SetGlobalMaximumNumberOfThreads(param.threads);
        }
        else
        {
            std::cout << "--> Executing with the default number of threads: " << itk::MultiThreader::GetGlobalDefaultNumberOfThreads() << std::endl;

        }*/
        // Some parameters may be specified as either vector or scalar, and need to be verified
        if (param.epsilon_per_level.size() != param.iter_per_level.size())
        {
          if (param.epsilon_per_level.size() == 1)
          {
            param.epsilon_per_level =
              std::vector<double>(param.iter_per_level.size(), param.epsilon_per_level.back());
          }
          else
          {
            throw GreedyException("--> Mismatch in size of vectors supplied with -n and -e options");
          }
        }

        auto fixedImageInfo = cbica::ImageInfo(fixedImage);
        auto movingImageInfo = cbica::ImageInfo(inputImageFiles[i]);

        if (fixedImageInfo.GetImageDimensions() != movingImageInfo.GetImageDimensions())
        {
          std::cerr << "--> Image dimensions do not match." << std::endl;
          return EXIT_FAILURE;
        }
        else
        {
          param.dim = fixedImageInfo.GetImageDimensions();
        }

        std::cout << "--> Applied transformation to moving image: " << outputImageFiles[i] << std::endl;

        if (param.flag_float_math)
        {
          switch (fixedImageInfo.GetImageDimensions())
          {
          case 2: GreedyRunner<2, float>::Run(param, cl); continue;
          case 3: GreedyRunner<3, float>::Run(param, cl); continue;
          case 4: GreedyRunner<4, float>::Run(param, cl); continue;
          default: throw GreedyException("--> Wrong number of dimensions requested: %d", param.dim);
          }
        }
        else
        {
          switch (fixedImageInfo.GetImageDimensions())
          {
          case 2: GreedyRunner<2, double>::Run(param, cl); continue;
          case 3: GreedyRunner<3, double>::Run(param, cl); continue;
          case 4: GreedyRunner<4, double>::Run(param, cl); continue;
          default: throw GreedyException("--> Wrong number of dimensions requested: %d", param.dim);
          }
        }

        std::cout << "--> Transformation complete " << std::endl;
      }
      return EXIT_SUCCESS;
    }
  }

}
