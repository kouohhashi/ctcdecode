#include "scorer.h"

#include <unistd.h>
#include <iostream>

#include <map>

#include "lm/config.hh"
#include "lm/model.hh"
#include "lm/state.hh"
#include "util/string_piece.hh"
#include "util/tokenize_piece.hh"

#include "decoder_utils.h"

using namespace lm::ngram;

Scorer::Scorer(double alpha,
               double beta,
               const std::string& lm_path,
               const std::vector<std::string>& vocab_list) {
  this->alpha = alpha;
  this->beta = beta;

  dictionary = nullptr;
  is_character_based_ = true;
  language_model_ = nullptr;

  max_order_ = 0;
  dict_size_ = 0;
  SPACE_ID_ = -1;

  setup(lm_path, vocab_list);
}

Scorer::~Scorer() {
  if (language_model_ != nullptr) {
    delete static_cast<lm::base::Model*>(language_model_);
  }
  if (dictionary != nullptr) {
    delete static_cast<fst::StdVectorFst*>(dictionary);
  }
}

void Scorer::setup(const std::string& lm_path,
                   const std::vector<std::string>& vocab_list) {

  // load language model
  load_lm(lm_path);

  // set char map for scorer
  set_char_map(vocab_list);

  // fill the dictionary for FST
  if (!is_character_based()) {
    fill_dictionary(true);
  }
}

void Scorer::load_lm(const std::string& lm_path) {
  const char* filename = lm_path.c_str();
  VALID_CHECK_EQ(access(filename, F_OK), 0, "Invalid language model path");

  RetriveStrEnumerateVocab enumerate;
  lm::ngram::Config config;
  config.enumerate_vocab = &enumerate;
  language_model_ = lm::ngram::LoadVirtual(filename, config);
  max_order_ = static_cast<lm::base::Model*>(language_model_)->Order();
  vocabulary_ = enumerate.vocabulary;

  for (size_t i = 0; i < vocabulary_.size(); ++i) {

    if (is_character_based_ && vocabulary_[i] != UNK_TOKEN &&
        vocabulary_[i] != START_TOKEN && vocabulary_[i] != END_TOKEN &&
        get_utf8_str_len(enumerate.vocabulary[i]) > 1) {
      is_character_based_ = false;
    }
  }
}

// get score with lm
double Scorer::get_log_cond_prob(const std::vector<std::string>& words,
                                 const std::map<std::string, std::string> &funnels) {

  lm::base::Model* model = static_cast<lm::base::Model*>(language_model_);

  double cond_prob;

  lm::ngram::State state, tmp_state, out_state;

  // avoid to inserting <s> in begin
  model->NullContextWrite(&state);

  for (size_t i = 0; i < words.size(); ++i) {

    lm::WordIndex word_index = model->BaseVocabulary().Index(words[i]);

    bool found_in_both = 0;
    bool found_in_funnel = 0;

    // try funnels
    if (word_index == 0) {

      // search words
      if (funnels.size() > 0) {

        if ( auto iter = funnels.find(words[i]); iter != end(funnels) ) {

          // // show log
          // std::ofstream ofs23334("/tmp/cpp_log.txt", std::ios::app);
          // ofs23334 << "words00: " << words[i] << std::endl;
          // ofs23334.close();

          found_in_funnel = 1;

          if (funnels.at(words[i]) != "default") {

            lm::WordIndex word_index2 = model->BaseVocabulary().Index(funnels.at(words[i]));
            word_index = word_index2;

          } else {
            // std::string score_of(funnels[words[i]]);
            lm::WordIndex word_index2 = model->BaseVocabulary().Index("ぱそこん");
            word_index = word_index2;
          }

        }
      }

    } else {
      if (funnels.size() > 0) {
        if ( auto iter = funnels.find(words[i]); iter != end(funnels) ) {

          // // show log
          // std::ofstream ofs23334("/tmp/cpp_log.txt", std::ios::app);
          // ofs23334 << "words2: " << words[i] << std::endl;
          // ofs23334.close();

          found_in_both = 1;
        }
      }
    }

    // encounter OOV
    if (word_index == 0) {

      //  -1000.0;
      return OOV_SCORE;
    }

    // this one get score from KenLM
    // input current state, new word index, and new state
    cond_prob = model->BaseScore(&state, word_index, &out_state);

    tmp_state = state;
    state = out_state;
    out_state = tmp_state;


    // if (found_in_funnel == true) {
    //
    //   // show log
    //   std::ofstream ofs23337("/tmp/cpp_log.txt", std::ios::app);
    //   ofs23337 << "cond_prob: " << cond_prob << " " << words[i] << std::endl;
    //   ofs23337.close();
    // }

    if (found_in_both == true) {

      // // show log
      // std::ofstream ofs23335("/tmp/cpp_log.txt", std::ios::app);
      // ofs23335 << "cond_prob1111: " << cond_prob << " " << words[i] << std::endl;
      // ofs23335.close();

      // 0.1 is just a number. you can experiment other numbers.
      cond_prob = cond_prob * 0.1;

    } else if (found_in_funnel == true) {

      // // show log
      // std::ofstream ofs23337("/tmp/cpp_log.txt", std::ios::app);
      // ofs23337 << "cond_prob2222: " << cond_prob << " " << words[i] << std::endl;
      // ofs23337.close();

      cond_prob = cond_prob * 0.05;

    }
  }

  // return  loge prob
  // NUM_FLT_LOGE = 0.4342944819;
  return cond_prob/NUM_FLT_LOGE;
}

double Scorer::get_sent_log_prob(const std::vector<std::string>& words,
                                 const std::map<std::string, std::string>& funnels) {
  std::vector<std::string> sentence;
  if (words.size() == 0) {
    for (size_t i = 0; i < max_order_; ++i) {
      sentence.push_back(START_TOKEN);
    }
  } else {
    for (size_t i = 0; i < max_order_ - 1; ++i) {
      sentence.push_back(START_TOKEN);
    }
    sentence.insert(sentence.end(), words.begin(), words.end());
  }
  sentence.push_back(END_TOKEN);
  return get_log_prob(sentence, funnels);
}

double Scorer::get_log_prob(const std::vector<std::string>& words,
                            const std::map<std::string, std::string>& funnels) {
  assert(words.size() > max_order_);
  double score = 0.0;
  for (size_t i = 0; i < words.size() - max_order_ + 1; ++i) {
    std::vector<std::string> ngram(words.begin() + i,
                                   words.begin() + i + max_order_);
    // score += get_log_cond_prob(ngram);
    score += get_log_cond_prob(ngram, funnels);
  }
  return score;
}

void Scorer::reset_params(float alpha, float beta) {
  this->alpha = alpha;
  this->beta = beta;
}

std::string Scorer::vec2str(const std::vector<int>& input) {
  std::string word;
  for (auto ind : input) {
    word += char_list_[ind];
  }
  return word;
}

std::vector<std::string> Scorer::split_labels(const std::vector<int>& labels) {
  if (labels.empty()) return {};

  std::string s = vec2str(labels);
  std::vector<std::string> words;
  if (is_character_based_) {
    words = split_utf8_str(s);
  } else {
    words = split_str(s, " ");
  }
  return words;
}

void Scorer::set_char_map(const std::vector<std::string>& char_list) {
  char_list_ = char_list;
  char_map_.clear();

  for (size_t i = 0; i < char_list_.size(); i++) {
    if (char_list_[i] == " ") {
      SPACE_ID_ = i;
    }
    // The initial state of FST is state 0, hence the index of chars in
    // the FST should start from 1 to avoid the conflict with the initial
    // state, otherwise wrong decoding results would be given.
    char_map_[char_list_[i]] = i + 1;
  }
}

std::vector<std::string> Scorer::make_ngram(PathTrie* prefix) {
  std::vector<std::string> ngram;
  PathTrie* current_node = prefix;
  PathTrie* new_node = nullptr;

  for (int order = 0; order < max_order_; order++) {
    std::vector<int> prefix_vec;
    std::vector<int> prefix_steps;

    if (is_character_based_) {
      new_node = current_node->get_path_vec(prefix_vec, prefix_steps, -1, 1);
      current_node = new_node;
    } else {
      new_node = current_node->get_path_vec(prefix_vec, prefix_steps, SPACE_ID_);
      current_node = new_node->parent;  // Skipping spaces
    }

    // reconstruct word
    std::string word = vec2str(prefix_vec);

    ngram.push_back(word);

    if (new_node->character == -1) {
      // No more spaces, but still need order
      for (int i = 0; i < max_order_ - order - 1; i++) {
        ngram.push_back(START_TOKEN);
      }
      break;
    }
  }
  std::reverse(ngram.begin(), ngram.end());
  return ngram;
}

void Scorer::fill_dictionary(bool add_space) {

  // http://www.openfst.org/twiki/bin/view/FST/FstQuickTour
  fst::StdVectorFst dictionary;

  // For each unigram convert to ints and put in trie
  int dict_size = 0;
  for (const auto& word : vocabulary_) {

    bool added = add_word_to_dictionary(
        word, char_map_, add_space, SPACE_ID_ + 1, &dictionary);
    dict_size += added ? 1 : 0;
  }

  // // can  i add word to dictinary manually ?
  // // > it worked!!!
  // bool added = add_word_to_dictionary(
  //     "きみしま", char_map_, add_space, SPACE_ID_ + 1, &dictionary);
  // dict_size += added ? 1 : 0;

  dict_size_ = dict_size;

  /* Simplify FST

   * This gets rid of "epsilon" transitions in the FST.
   * These are transitions that don't require a string input to be taken.
   * Getting rid of them is necessary to make the FST determinisitc, but
   * can greatly increase the size of the FST
   */
  fst::RmEpsilon(&dictionary);
  fst::StdVectorFst* new_dict = new fst::StdVectorFst;

  /* This makes the FST deterministic, meaning for any string input there's
   * only one possible state the FST could be in.  It is assumed our
   * dictionary is deterministic when using it.
   * (lest we'd have to check for multiple transitions at each state)
   */
  fst::Determinize(dictionary, new_dict);




  // // can  i add word to dictinary manually ? HERE AFTER Determinize?
  // // > it worked!!!
  // bool added = add_word_to_dictionary(
  //     "きみしま", char_map_, add_space, SPACE_ID_ + 1, &dictionary);
  // dict_size += added ? 1 : 0;




  /* Finds the simplest equivalent fst. This is unnecessary but decreases
   * memory usage of the dictionary
   */
  fst::Minimize(new_dict);

  // HOW ABOUT HERE???
  // can  i add word to dictinary manually ? HERE AFTER Determinize?
  // > it worked!!!
  bool added = add_word_to_dictionary(
      "きみしま", char_map_, add_space, SPACE_ID_ + 1, &dictionary);
  dict_size += added ? 1 : 0;


  bool added2 = add_word_to_dictionary(
      "きみしま", char_map_, add_space, SPACE_ID_ + 1, &dictionary);
  dict_size += added2 ? 1 : 0;

  this->dictionary = new_dict;

  // not working...
  // // fst::StdVectorFst* new_dict2 = dictionary
  // this->dictionary = *dictionary;
}
