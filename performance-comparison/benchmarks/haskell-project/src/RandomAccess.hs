-- random_access -- (p & 0xFF) file, LCG-driven single-byte seeks, sum.
{-# LANGUAGE BangPatterns #-}
import System.Environment (getArgs)
import System.IO
import System.Directory (removeFile)
import qualified Data.ByteString as BS
import Data.Word (Word64)
lcgA, lcgC :: Word64
lcgA = 6364136223846793005
lcgC = 1442695040888963407
main :: IO ()
main = do
  args <- getArgs
  let (fileSize, nReads) = case args of
        (a:b:_) -> (read a, read b)
        (a:_)   -> (read a, 1000)
        _       -> (1048576, 1000) :: (Word64, Int)
      path = "/tmp/bench_io_random_hs.bin"
  h <- openBinaryFile path WriteMode
  let put !pos
        | pos >= fileSize = return ()
        | otherwise = do
            let c = fromIntegral (min 4096 (fileSize - pos)) :: Int
            BS.hPut h (BS.pack [ fromIntegral ((pos + fromIntegral i) `mod` 256)
                               | i <- [0 .. c - 1] ])
            put (pos + fromIntegral c)
  put 0
  hClose h
  r <- openBinaryFile path ReadMode
  let go !i !st !checksum
        | i >= nReads = return checksum
        | otherwise = do
            let s = st * lcgA + lcgC
            hSeek r AbsoluteSeek (fromIntegral ((s `div` 2) `mod` fileSize))
            b <- BS.hGet r 1
            let add = if BS.null b then 0 else fromIntegral (BS.head b)
            go (i + 1) s (checksum + add)
  checksum <- go 0 12345678 (0 :: Int)
  hClose r
  removeFile path
  print checksum
